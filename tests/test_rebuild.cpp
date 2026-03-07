#include "helpers/test_helpers.h"
#include "../src/common/types.h"
#include "../src/common/log.h"
#include "../src/tracer/trace_session.h"
#include "../src/tracer/ptrace_backend.h"

#include <unistd.h>
#include <string>
#include <vector>
#include <set>

using namespace bdtrace;

static const char* TEST_DB = "/tmp/bdtrace_test_rebuild.db";

static void run(const char* cmd) {
    int ret = system(cmd);
    (void)ret;
}

static void cleanup() {
    unlink(TEST_DB);
    run("rm -rf /tmp/bdtrace_rebuild_work");
}

static void setup_workdir() {
    run("mkdir -p /tmp/bdtrace_rebuild_work/src");
}

// Helper: trace a shell command and return the DB
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

// Helper: collect PIDs that have output files (leaf output producers)
// This mirrors the rebuild algorithm's display filter
static std::set<int> get_leaf_output_pids(Database& db) {
    std::vector<FileAccessRecord> all_acc;
    std::vector<ProcessRecord> all_procs;
    db.get_all_file_accesses(all_acc);
    db.get_all_processes(all_procs);

    std::map<int, std::set<std::string> > pid_to_outputs;
    std::map<int, ProcessRecord> proc_map;

    for (size_t i = 0; i < all_acc.size(); ++i) {
        int m = all_acc[i].mode;
        if (m == FA_WRITE || m == FA_RDWR || m == FA_RENAME_DST
            || m == FA_LINK_DST || m == FA_SYMLINK_LINK
            || m == FA_TRUNCATE || m == FA_MKNOD || m == FA_MKDIR) {
            pid_to_outputs[all_acc[i].pid].insert(all_acc[i].filename);
        }
    }
    for (size_t i = 0; i < all_procs.size(); ++i) {
        proc_map[all_procs[i].pid] = all_procs[i];
    }

    // Find PIDs with outputs whose parent also has outputs (non-leaf)
    std::set<int> has_output_child;
    for (std::map<int, std::set<std::string> >::const_iterator it = pid_to_outputs.begin();
         it != pid_to_outputs.end(); ++it) {
        std::map<int, ProcessRecord>::const_iterator pit = proc_map.find(it->first);
        if (pit == proc_map.end()) continue;
        int ppid = pit->second.ppid;
        if (pid_to_outputs.find(ppid) != pid_to_outputs.end()) {
            has_output_child.insert(ppid);
        }
    }

    std::set<int> result;
    for (std::map<int, std::set<std::string> >::const_iterator it = pid_to_outputs.begin();
         it != pid_to_outputs.end(); ++it) {
        if (has_output_child.find(it->first) == has_output_child.end()) {
            result.insert(it->first);
        }
    }
    return result;
}

// Helper: get cmdline basename for a PID
static std::string get_cmd_name(Database& db, int pid) {
    ProcessRecord rec;
    if (!db.get_process(pid, rec)) return "";
    std::string first = rec.cmdline;
    size_t sp = first.find(' ');
    if (sp != std::string::npos) first = first.substr(0, sp);
    size_t sl = first.rfind('/');
    if (sl != std::string::npos) first = first.substr(sl + 1);
    return first;
}

// Helper: get all cmd names for a set of PIDs
static std::set<std::string> get_cmd_names(Database& db, const std::set<int>& pids) {
    std::set<std::string> names;
    for (std::set<int>::const_iterator it = pids.begin(); it != pids.end(); ++it) {
        names.insert(get_cmd_name(db, *it));
    }
    return names;
}

// -----------------------------------------------------------------
// Test 1: Multi-level shell nesting
//   sh -> sh -> sh -> cp
// The intermediate shells should not appear, only cp (leaf output producer)
// -----------------------------------------------------------------
void test_nested_shells() {
    cleanup();
    setup_workdir();

    // Create source file
    run("echo 'hello' > /tmp/bdtrace_rebuild_work/src/input.txt");

    // 3 levels of sh nesting, final command is cp
    std::string cmd =
        "sh -c \"sh -c \\\"cp /tmp/bdtrace_rebuild_work/src/input.txt"
        " /tmp/bdtrace_rebuild_work/output.txt\\\"\"";

    ASSERT_TRUE(trace_cmd(cmd));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);

    // Should have at least 4 processes (sh -> sh -> sh -> cp)
    ASSERT_TRUE((int)procs.size() >= 4);

    // Only cp should be a leaf output producer (writes output.txt)
    std::set<int> leaf_pids = get_leaf_output_pids(db);
    std::set<std::string> names = get_cmd_names(db, leaf_pids);

    // sh should NOT be in leaf output producers
    ASSERT_TRUE(names.find("sh") == names.end());
    // cp should be present
    ASSERT_TRUE(names.find("cp") != names.end());

    cleanup();
}

// -----------------------------------------------------------------
// Test 2: File dependency chain
//   Step 1: Read A, write B (cat A > B)
//   Step 2: Read B and C, write D (cat B C > D)
// Changing A should affect both steps.
// Changing C should affect only step 2.
// -----------------------------------------------------------------
void test_dependency_chain() {
    cleanup();
    setup_workdir();

    // Create source files
    run("echo 'aaa' > /tmp/bdtrace_rebuild_work/src/a.txt");
    run("echo 'ccc' > /tmp/bdtrace_rebuild_work/src/c.txt");

    std::string cmd =
        "cat /tmp/bdtrace_rebuild_work/src/a.txt > /tmp/bdtrace_rebuild_work/b.txt && "
        "cat /tmp/bdtrace_rebuild_work/b.txt /tmp/bdtrace_rebuild_work/src/c.txt"
        " > /tmp/bdtrace_rebuild_work/d.txt";

    ASSERT_TRUE(trace_cmd(cmd));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    // Verify file accesses exist
    std::vector<FileAccessRecord> all_acc;
    db.get_all_file_accesses(all_acc);

    bool found_a_read = false, found_b_write = false;
    bool found_b_read = false, found_d_write = false;
    for (size_t i = 0; i < all_acc.size(); ++i) {
        if (all_acc[i].filename.find("a.txt") != std::string::npos
            && (all_acc[i].mode == FA_READ)) found_a_read = true;
        if (all_acc[i].filename.find("b.txt") != std::string::npos
            && (all_acc[i].mode == FA_WRITE)) found_b_write = true;
        if (all_acc[i].filename.find("b.txt") != std::string::npos
            && (all_acc[i].mode == FA_READ)) found_b_read = true;
        if (all_acc[i].filename.find("d.txt") != std::string::npos
            && (all_acc[i].mode == FA_WRITE)) found_d_write = true;
    }
    ASSERT_TRUE(found_a_read);
    ASSERT_TRUE(found_b_write);
    ASSERT_TRUE(found_b_read);
    ASSERT_TRUE(found_d_write);

    // The dependency chain: a.txt -> [sh] -> b.txt -> [sh] -> d.txt
    //                                          c.txt -> [sh] -> d.txt
    // sh performs redirects (open()), so sh is the output producer.
    // Since it's a single sh -c "... && ...", one sh writes both b.txt and d.txt.
    std::set<int> leaf_pids = get_leaf_output_pids(db);
    ASSERT_TRUE((int)leaf_pids.size() >= 1);

    // The single leaf output producer should have written both b.txt and d.txt
    bool wrote_b = false, wrote_d = false;
    for (size_t i = 0; i < all_acc.size(); ++i) {
        if (leaf_pids.find(all_acc[i].pid) == leaf_pids.end()) continue;
        if (all_acc[i].filename.find("b.txt") != std::string::npos
            && all_acc[i].mode == FA_WRITE) wrote_b = true;
        if (all_acc[i].filename.find("d.txt") != std::string::npos
            && all_acc[i].mode == FA_WRITE) wrote_d = true;
    }
    ASSERT_TRUE(wrote_b);
    ASSERT_TRUE(wrote_d);

    cleanup();
}

// -----------------------------------------------------------------
// Test 3: Pipe (output doesn't change)
//   cat A | sort > B
// The pipe creates a reader (cat) and a writer (sort via sh redirect).
// cat reads A but its stdout goes to pipe (not a file).
// sort's output goes to B via redirect.
// Changing A should affect this pipeline.
// -----------------------------------------------------------------
void test_pipe_redirect() {
    cleanup();
    setup_workdir();

    run("printf 'cherry\\napple\\nbanana\\n' > /tmp/bdtrace_rebuild_work/src/fruits.txt");

    std::string cmd =
        "cat /tmp/bdtrace_rebuild_work/src/fruits.txt | sort"
        " > /tmp/bdtrace_rebuild_work/sorted.txt";

    ASSERT_TRUE(trace_cmd(cmd));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    std::vector<FileAccessRecord> all_acc;
    db.get_all_file_accesses(all_acc);

    // Verify: fruits.txt was read, sorted.txt was written
    bool found_fruit_read = false, found_sorted_write = false;
    for (size_t i = 0; i < all_acc.size(); ++i) {
        if (all_acc[i].filename.find("fruits.txt") != std::string::npos
            && all_acc[i].mode == FA_READ) found_fruit_read = true;
        if (all_acc[i].filename.find("sorted.txt") != std::string::npos
            && all_acc[i].mode == FA_WRITE) found_sorted_write = true;
    }
    ASSERT_TRUE(found_fruit_read);
    ASSERT_TRUE(found_sorted_write);

    // Leaf output producers should include the process writing sorted.txt
    // but NOT cat (cat writes to pipe, not a file)
    std::set<int> leaf_pids = get_leaf_output_pids(db);
    ASSERT_TRUE(!leaf_pids.empty());

    // cat should not be a leaf output producer (pipes are not tracked as files)
    std::set<std::string> names = get_cmd_names(db, leaf_pids);
    ASSERT_TRUE(names.find("cat") == names.end());

    cleanup();
}

// -----------------------------------------------------------------
// Test 4: Redirect without command change
//   echo "data" > out.txt
// The shell (sh) itself performs the redirect and write.
// sh should appear as a leaf output producer since it writes the file.
// -----------------------------------------------------------------
void test_shell_redirect() {
    cleanup();
    setup_workdir();

    std::string cmd = "echo hello > /tmp/bdtrace_rebuild_work/out.txt";

    ASSERT_TRUE(trace_cmd(cmd));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    std::vector<FileAccessRecord> all_acc;
    db.get_all_file_accesses(all_acc);

    bool found_write = false;
    for (size_t i = 0; i < all_acc.size(); ++i) {
        if (all_acc[i].filename.find("out.txt") != std::string::npos
            && (all_acc[i].mode == FA_WRITE)) found_write = true;
    }
    ASSERT_TRUE(found_write);

    // A process writing out.txt should exist in leaf output producers
    std::set<int> leaf_pids = get_leaf_output_pids(db);
    ASSERT_TRUE(!leaf_pids.empty());

    cleanup();
}

// -----------------------------------------------------------------
// Test 5: Multi-step build simulation
//   Step 1: Generate header: echo "#define VER 1" > gen.h
//   Step 2: "Compile" using header: cat gen.h src.c > combined.o
//   Step 3: "Link": cat combined.o > app
// This simulates: code generation -> compile -> link
// Changing src.c should affect steps 2 and 3.
// -----------------------------------------------------------------
void test_build_simulation() {
    cleanup();
    setup_workdir();

    run("echo 'int main(){}' > /tmp/bdtrace_rebuild_work/src/src.c");

    std::string cmd =
        "echo '#define VER 1' > /tmp/bdtrace_rebuild_work/gen.h && "
        "cat /tmp/bdtrace_rebuild_work/gen.h /tmp/bdtrace_rebuild_work/src/src.c"
        " > /tmp/bdtrace_rebuild_work/combined.o && "
        "cat /tmp/bdtrace_rebuild_work/combined.o > /tmp/bdtrace_rebuild_work/app";

    ASSERT_TRUE(trace_cmd(cmd));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    std::vector<FileAccessRecord> all_acc;
    db.get_all_file_accesses(all_acc);

    // Verify the dependency chain exists in the trace
    bool found_src_read = false, found_genh_write = false;
    bool found_combined_write = false, found_app_write = false;
    for (size_t i = 0; i < all_acc.size(); ++i) {
        const std::string& fn = all_acc[i].filename;
        if (fn.find("src.c") != std::string::npos && all_acc[i].mode == FA_READ)
            found_src_read = true;
        if (fn.find("gen.h") != std::string::npos && all_acc[i].mode == FA_WRITE)
            found_genh_write = true;
        if (fn.find("combined.o") != std::string::npos && all_acc[i].mode == FA_WRITE)
            found_combined_write = true;
        if (fn.find("app") != std::string::npos && all_acc[i].mode == FA_WRITE)
            found_app_write = true;
    }
    ASSERT_TRUE(found_src_read);
    ASSERT_TRUE(found_genh_write);
    ASSERT_TRUE(found_combined_write);
    ASSERT_TRUE(found_app_write);

    // sh performs all redirects, so one sh process writes gen.h, combined.o, app
    std::set<int> leaf_pids = get_leaf_output_pids(db);
    ASSERT_TRUE((int)leaf_pids.size() >= 1);

    // The leaf output producer(s) should have written all 3 output files
    bool wrote_gen = false, wrote_combined = false, wrote_app = false;
    for (size_t i = 0; i < all_acc.size(); ++i) {
        if (leaf_pids.find(all_acc[i].pid) == leaf_pids.end()) continue;
        if (all_acc[i].mode != FA_WRITE) continue;
        if (all_acc[i].filename.find("gen.h") != std::string::npos) wrote_gen = true;
        if (all_acc[i].filename.find("combined.o") != std::string::npos) wrote_combined = true;
        if (all_acc[i].filename.find("/app") != std::string::npos) wrote_app = true;
    }
    ASSERT_TRUE(wrote_gen);
    ASSERT_TRUE(wrote_combined);
    ASSERT_TRUE(wrote_app);

    cleanup();
}

// -----------------------------------------------------------------
// Test 6: Folder prefix match
//   Multiple files under src/ are read.
//   Verifying that the trace captures reads from src/ subdirectory.
// -----------------------------------------------------------------
void test_folder_prefix() {
    cleanup();
    setup_workdir();

    run("echo 'aaa' > /tmp/bdtrace_rebuild_work/src/file1.txt");
    run("echo 'bbb' > /tmp/bdtrace_rebuild_work/src/file2.txt");

    std::string cmd =
        "cat /tmp/bdtrace_rebuild_work/src/file1.txt > /tmp/bdtrace_rebuild_work/out1.txt && "
        "cat /tmp/bdtrace_rebuild_work/src/file2.txt > /tmp/bdtrace_rebuild_work/out2.txt";

    ASSERT_TRUE(trace_cmd(cmd));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    std::vector<FileAccessRecord> all_acc;
    db.get_all_file_accesses(all_acc);

    // Both src/ files should be read
    bool found_f1 = false, found_f2 = false;
    for (size_t i = 0; i < all_acc.size(); ++i) {
        if (all_acc[i].filename.find("src/file1.txt") != std::string::npos
            && all_acc[i].mode == FA_READ) found_f1 = true;
        if (all_acc[i].filename.find("src/file2.txt") != std::string::npos
            && all_acc[i].mode == FA_READ) found_f2 = true;
    }
    ASSERT_TRUE(found_f1);
    ASSERT_TRUE(found_f2);

    // sh performs both redirects, so one sh process writes both outputs
    std::set<int> leaf_pids = get_leaf_output_pids(db);
    ASSERT_TRUE((int)leaf_pids.size() >= 1);

    // The leaf output producer(s) should have written both out1.txt and out2.txt
    bool wrote_o1 = false, wrote_o2 = false;
    for (size_t i = 0; i < all_acc.size(); ++i) {
        if (leaf_pids.find(all_acc[i].pid) == leaf_pids.end()) continue;
        if (all_acc[i].mode != FA_WRITE) continue;
        if (all_acc[i].filename.find("out1.txt") != std::string::npos) wrote_o1 = true;
        if (all_acc[i].filename.find("out2.txt") != std::string::npos) wrote_o2 = true;
    }
    ASSERT_TRUE(wrote_o1);
    ASSERT_TRUE(wrote_o2);

    cleanup();
}

void run_rebuild_tests() {
    std::printf("=== Rebuild Logic Tests ===\n");

    RUN_TEST(test_nested_shells);
    RUN_TEST(test_dependency_chain);
    RUN_TEST(test_pipe_redirect);
    RUN_TEST(test_shell_redirect);
    RUN_TEST(test_build_simulation);
    RUN_TEST(test_folder_prefix);
}
