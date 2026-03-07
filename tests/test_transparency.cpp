#include "helpers/test_helpers.h"
#include "../src/common/types.h"
#include "../src/common/log.h"
#include "../src/tracer/trace_session.h"
#include "../src/tracer/ptrace_backend.h"

#include <unistd.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <cstdio>

using namespace bdtrace;

static const char* TEST_DB = "/tmp/bdtrace_test_transparency.db";

static void run(const char* cmd) {
    int ret = system(cmd);
    (void)ret;
}

static void cleanup() {
    unlink(TEST_DB);
    run("rm -rf /tmp/bdtrace_tp_work");
}

static void setup() {
    run("mkdir -p /tmp/bdtrace_tp_work");
}

// Helper: read entire file content into string
static std::string read_file(const char* path) {
    std::string result;
    FILE* f = std::fopen(path, "r");
    if (!f) return result;
    char buf[1024];
    while (size_t n = std::fread(buf, 1, sizeof(buf), f)) {
        result.append(buf, n);
    }
    std::fclose(f);
    return result;
}

// Helper: trace a command, return root process exit code from DB
static int trace_and_get_exit(const std::vector<std::string>& argv) {
    TraceSession session;
    if (!session.open(TEST_DB)) return -999;

    PtraceBackend backend(session);
    if (backend.start(argv) != 0) return -999;
    backend.run_event_loop();

    // Find the root process (the one whose ppid is not in the traced set)
    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);
    std::set<int> pids;
    for (size_t i = 0; i < procs.size(); ++i) pids.insert(procs[i].pid);
    for (size_t i = 0; i < procs.size(); ++i) {
        if (pids.find(procs[i].ppid) == pids.end()) {
            return procs[i].exit_code;
        }
    }
    return -999;
}

// =================================================================
// Transparency: exit code preservation
// =================================================================

// exit 0
void test_tp_exit_zero() {
    cleanup();
    std::vector<std::string> argv;
    argv.push_back("/bin/true");
    ASSERT_EQ(trace_and_get_exit(argv), 0);
    cleanup();
}

// exit 1
void test_tp_exit_one() {
    cleanup();
    std::vector<std::string> argv;
    argv.push_back("/bin/false");
    ASSERT_EQ(trace_and_get_exit(argv), 1);
    cleanup();
}

// arbitrary exit code: exit 42
void test_tp_exit_arbitrary() {
    cleanup();
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("exit 42");
    ASSERT_EQ(trace_and_get_exit(argv), 42);
    cleanup();
}

// exit code propagation through shell: false || exit 7
void test_tp_exit_conditional() {
    cleanup();
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("/bin/false || exit 7");
    ASSERT_EQ(trace_and_get_exit(argv), 7);
    cleanup();
}

// =================================================================
// Transparency: stdout content preservation
// =================================================================

// echo output to file via redirect
void test_tp_stdout_echo() {
    cleanup();
    setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("echo 'hello world' > /tmp/bdtrace_tp_work/echo_out.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/echo_out.txt");
    ASSERT_STR_EQ(content.c_str(), "hello world\n");

    cleanup();
}

// printf with special characters
void test_tp_stdout_special_chars() {
    cleanup();
    setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("printf 'line1\\nline2\\ttab\\n' > /tmp/bdtrace_tp_work/special.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/special.txt");
    ASSERT_STR_EQ(content.c_str(), "line1\nline2\ttab\n");

    cleanup();
}

// =================================================================
// Transparency: file content preservation
// =================================================================

// cp should produce identical file
void test_tp_file_cp() {
    cleanup();
    setup();

    run("printf 'alpha\\nbeta\\ngamma\\n' > /tmp/bdtrace_tp_work/original.txt");

    std::vector<std::string> argv;
    argv.push_back("cp");
    argv.push_back("/tmp/bdtrace_tp_work/original.txt");
    argv.push_back("/tmp/bdtrace_tp_work/copy.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string orig = read_file("/tmp/bdtrace_tp_work/original.txt");
    std::string copy = read_file("/tmp/bdtrace_tp_work/copy.txt");
    ASSERT_STR_EQ(orig.c_str(), copy.c_str());

    cleanup();
}

// cat concatenation should produce correct output
void test_tp_file_cat_concat() {
    cleanup();
    setup();

    run("echo 'AAA' > /tmp/bdtrace_tp_work/part1.txt");
    run("echo 'BBB' > /tmp/bdtrace_tp_work/part2.txt");

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("cat /tmp/bdtrace_tp_work/part1.txt /tmp/bdtrace_tp_work/part2.txt"
                    " > /tmp/bdtrace_tp_work/merged.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/merged.txt");
    ASSERT_STR_EQ(content.c_str(), "AAA\nBBB\n");

    cleanup();
}

// sort should produce correctly sorted output
void test_tp_file_sort() {
    cleanup();
    setup();

    run("printf 'cherry\\napple\\nbanana\\n' > /tmp/bdtrace_tp_work/unsorted.txt");

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sort /tmp/bdtrace_tp_work/unsorted.txt"
                    " > /tmp/bdtrace_tp_work/sorted.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/sorted.txt");
    ASSERT_STR_EQ(content.c_str(), "apple\nbanana\ncherry\n");

    cleanup();
}

// =================================================================
// Transparency: pipe data integrity
// =================================================================

// echo | cat should pass data through unchanged
void test_tp_pipe_passthrough() {
    cleanup();
    setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("echo 'pipe_test_data' | cat > /tmp/bdtrace_tp_work/pipe_out.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/pipe_out.txt");
    ASSERT_STR_EQ(content.c_str(), "pipe_test_data\n");

    cleanup();
}

// multi-stage pipe: echo | tr | sort
void test_tp_pipe_multi_stage() {
    cleanup();
    setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("printf 'C\\nA\\nB\\n' | tr 'A-C' 'a-c' | sort"
                    " > /tmp/bdtrace_tp_work/pipe_multi.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/pipe_multi.txt");
    ASSERT_STR_EQ(content.c_str(), "a\nb\nc\n");

    cleanup();
}

// =================================================================
// Transparency: stderr preservation
// =================================================================

// stderr should be preserved (ls on nonexistent file)
void test_tp_stderr() {
    cleanup();
    setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("ls /tmp/bdtrace_tp_nonexistent_xyz 2> /tmp/bdtrace_tp_work/stderr.txt;"
                    " true");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/stderr.txt");
    // stderr should contain error message about the nonexistent path
    ASSERT_TRUE(content.size() > 0);
    ASSERT_TRUE(content.find("No such file") != std::string::npos
                || content.find("cannot access") != std::string::npos);

    cleanup();
}

// =================================================================
// Data integrity: timing and process consistency
// =================================================================

void test_integrity_timing() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("/bin/true && /bin/echo done > /tmp/bdtrace_tp_work/x");

    setup();
    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);

    for (size_t i = 0; i < procs.size(); ++i) {
        // All completed processes: start < end
        if (procs[i].end_time_us > 0) {
            ASSERT_TRUE(procs[i].start_time_us > 0);
            ASSERT_TRUE(procs[i].start_time_us <= procs[i].end_time_us);
        }
        // PID should be positive
        ASSERT_TRUE(procs[i].pid > 0);
        // cmdline should not be empty
        ASSERT_TRUE(!procs[i].cmdline.empty());
    }

    cleanup();
}

// All file access PIDs should reference a known process
void test_integrity_file_access_pids() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("cat /etc/hostname > /tmp/bdtrace_tp_work/host.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);
    std::set<int> known_pids;
    for (size_t i = 0; i < procs.size(); ++i) {
        known_pids.insert(procs[i].pid);
    }

    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    ASSERT_TRUE(!accesses.empty());

    for (size_t i = 0; i < accesses.size(); ++i) {
        // Every file access PID must exist in the process table
        ASSERT_TRUE(known_pids.find(accesses[i].pid) != known_pids.end());
        // filename should not be empty
        ASSERT_TRUE(!accesses[i].filename.empty());
    }

    cleanup();
}

// Parent-child relationships: children's ppid should be in process set
void test_integrity_parent_child() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sh -c '/bin/true' && sh -c '/bin/false'; true");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);
    ASSERT_TRUE((int)procs.size() >= 3);

    std::set<int> pids;
    for (size_t i = 0; i < procs.size(); ++i) {
        pids.insert(procs[i].pid);
    }

    // All non-root processes' ppid should be in the traced set
    // (root's ppid is external, so skip those)
    int orphan_count = 0;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (pids.find(procs[i].ppid) == pids.end()) {
            ++orphan_count;
        }
    }
    // Exactly 1 root process (the initial sh)
    ASSERT_EQ(orphan_count, 1);

    cleanup();
}

// =================================================================
// Edge cases
// =================================================================

// Many short-lived processes
void test_edge_rapid_forks() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    // Spawn 20 short-lived processes sequentially
    std::string cmd;
    for (int i = 0; i < 20; ++i) {
        if (i > 0) cmd += " && ";
        cmd += "/bin/true";
    }
    argv.push_back(cmd);

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);
    // sh + 20 true processes
    ASSERT_TRUE((int)procs.size() >= 21);

    // All should have exit code 0
    for (size_t i = 0; i < procs.size(); ++i) {
        ASSERT_EQ(procs[i].exit_code, 0);
    }

    cleanup();
}

// Process that produces no output files
void test_edge_no_output() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("/bin/true");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // Check that we still get process records even with no file writes
    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);
    ASSERT_TRUE(!procs.empty());

    // No write-mode file accesses should exist
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    for (size_t i = 0; i < accesses.size(); ++i) {
        ASSERT_TRUE(accesses[i].mode != FA_WRITE);
    }

    cleanup();
}

// Deep shell nesting: sh -> sh -> sh -> sh -> /bin/true
void test_edge_deep_nesting() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sh -c \"sh -c \\\"sh -c /bin/true\\\"\"");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);
    // At least 5 processes: 4x sh + 1x true
    ASSERT_TRUE((int)procs.size() >= 5);

    // Verify depth: find the process whose ppid chain length is >= 4
    std::map<int, ProcessRecord> pmap;
    for (size_t i = 0; i < procs.size(); ++i) {
        pmap[procs[i].pid] = procs[i];
    }

    int max_depth = 0;
    for (size_t i = 0; i < procs.size(); ++i) {
        int depth = 0;
        int cur = procs[i].pid;
        std::set<int> visited;
        while (pmap.find(cur) != pmap.end() && visited.find(cur) == visited.end()) {
            visited.insert(cur);
            cur = pmap[cur].ppid;
            ++depth;
        }
        if (depth > max_depth) max_depth = depth;
    }
    ASSERT_TRUE(max_depth >= 4);

    cleanup();
}

// Append mode: >> should not overwrite existing content
void test_edge_append_mode() {
    cleanup();
    setup();

    run("echo 'first' > /tmp/bdtrace_tp_work/append.txt");

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("echo 'second' >> /tmp/bdtrace_tp_work/append.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/append.txt");
    ASSERT_STR_EQ(content.c_str(), "first\nsecond\n");

    cleanup();
}

// Empty file creation
void test_edge_empty_file() {
    cleanup();
    setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("touch /tmp/bdtrace_tp_work/empty.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    // File should exist and be empty
    std::string content = read_file("/tmp/bdtrace_tp_work/empty.txt");
    ASSERT_STR_EQ(content.c_str(), "");

    cleanup();
}

// Concurrent background processes should all be captured
void test_edge_background_procs() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back(
        "echo 'a' > /tmp/bdtrace_tp_work/bg1.txt &"
        " echo 'b' > /tmp/bdtrace_tp_work/bg2.txt &"
        " echo 'c' > /tmp/bdtrace_tp_work/bg3.txt &"
        " wait");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // All 3 files should exist with correct content
    ASSERT_STR_EQ(read_file("/tmp/bdtrace_tp_work/bg1.txt").c_str(), "a\n");
    ASSERT_STR_EQ(read_file("/tmp/bdtrace_tp_work/bg2.txt").c_str(), "b\n");
    ASSERT_STR_EQ(read_file("/tmp/bdtrace_tp_work/bg3.txt").c_str(), "c\n");

    // All 3 writes should be recorded in DB
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    int bg_writes = 0;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].mode == FA_WRITE
            && accesses[i].filename.find("bdtrace_tp_work/bg") != std::string::npos) {
            ++bg_writes;
        }
    }
    ASSERT_TRUE(bg_writes >= 3);

    cleanup();
}

// =================================================================
// Transparency: signal handling
// =================================================================

// SIGTERM to child: child should be killed and exit recorded
void test_tp_signal_term() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    // trap SIGTERM, write marker, then exit
    argv.push_back("trap 'echo caught > /tmp/bdtrace_tp_work/sig.txt; exit 0' TERM;"
                    " sleep 10 & SPID=$!; kill -TERM $$; wait $SPID 2>/dev/null; true");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::string content = read_file("/tmp/bdtrace_tp_work/sig.txt");
    ASSERT_STR_EQ(content.c_str(), "caught\n");

    cleanup();
}

// SIGUSR1 handler should work under tracing
void test_tp_signal_usr1() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("trap 'echo usr1 > /tmp/bdtrace_tp_work/usr1.txt' USR1;"
                    " kill -USR1 $$; exit 0");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::string content = read_file("/tmp/bdtrace_tp_work/usr1.txt");
    ASSERT_STR_EQ(content.c_str(), "usr1\n");

    cleanup();
}

// =================================================================
// Transparency: environment variables
// =================================================================

// Environment variable passed to child should be readable
void test_tp_env_inherited() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("BDTRACE_TEST_VAR=hello_world sh -c"
                    " 'echo $BDTRACE_TEST_VAR > /tmp/bdtrace_tp_work/env.txt'");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::string content = read_file("/tmp/bdtrace_tp_work/env.txt");
    ASSERT_STR_EQ(content.c_str(), "hello_world\n");

    cleanup();
}

// PATH should be functional (commands found via PATH)
void test_tp_env_path() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    // Use 'which' or 'command -v' to verify PATH works
    argv.push_back("command -v cat > /tmp/bdtrace_tp_work/path.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::string content = read_file("/tmp/bdtrace_tp_work/path.txt");
    // Should contain a path to cat (e.g., /usr/bin/cat or /bin/cat)
    ASSERT_TRUE(content.find("cat") != std::string::npos);

    cleanup();
}

// =================================================================
// Many file accesses: completeness
// =================================================================

// Create and read 50 files, verify all are recorded
void test_edge_many_files() {
    cleanup();
    setup();
    run("mkdir -p /tmp/bdtrace_tp_work/many");

    // Create 50 files before tracing
    for (int i = 0; i < 50; ++i) {
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd),
                      "echo 'data%d' > /tmp/bdtrace_tp_work/many/f%03d.txt", i, i);
        run(cmd);
    }

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    // cat all 50 files into one output
    argv.push_back("cat /tmp/bdtrace_tp_work/many/f*.txt"
                    " > /tmp/bdtrace_tp_work/many_out.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // Verify output file has 50 lines
    std::string content = read_file("/tmp/bdtrace_tp_work/many_out.txt");
    int line_count = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') ++line_count;
    }
    ASSERT_EQ(line_count, 50);

    // Verify file accesses recorded: at least 50 reads from many/ directory
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    int many_reads = 0;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("bdtrace_tp_work/many/f") != std::string::npos
            && accesses[i].mode == FA_READ) {
            ++many_reads;
        }
    }
    ASSERT_TRUE(many_reads >= 50);

    cleanup();
}

// =================================================================
// Symlink and hardlink operations
// =================================================================

// Symlink creation should be recorded
void test_edge_symlink() {
    cleanup();
    setup();

    run("echo 'target_data' > /tmp/bdtrace_tp_work/link_target.txt");

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("ln -s /tmp/bdtrace_tp_work/link_target.txt"
                    " /tmp/bdtrace_tp_work/sym.txt &&"
                    " cat /tmp/bdtrace_tp_work/sym.txt"
                    " > /tmp/bdtrace_tp_work/sym_out.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // Symlink should work: reading through symlink gives target content
    std::string content = read_file("/tmp/bdtrace_tp_work/sym_out.txt");
    ASSERT_STR_EQ(content.c_str(), "target_data\n");

    // Check that symlink operation is recorded in DB
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    bool found_symlink = false;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("sym.txt") != std::string::npos
            && (accesses[i].mode == FA_SYMLINK_LINK)) {
            found_symlink = true;
        }
    }
    ASSERT_TRUE(found_symlink);

    cleanup();
}

// Hardlink creation should be recorded
void test_edge_hardlink() {
    cleanup();
    setup();

    run("echo 'hardlink_data' > /tmp/bdtrace_tp_work/hl_src.txt");

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("ln /tmp/bdtrace_tp_work/hl_src.txt"
                    " /tmp/bdtrace_tp_work/hl_dst.txt &&"
                    " cat /tmp/bdtrace_tp_work/hl_dst.txt"
                    " > /tmp/bdtrace_tp_work/hl_out.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // Hardlink should work: reading gives same content
    std::string content = read_file("/tmp/bdtrace_tp_work/hl_out.txt");
    ASSERT_STR_EQ(content.c_str(), "hardlink_data\n");

    // Check link operations recorded
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    bool found_link_dst = false;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("hl_dst.txt") != std::string::npos
            && accesses[i].mode == FA_LINK_DST) {
            found_link_dst = true;
        }
    }
    ASSERT_TRUE(found_link_dst);

    cleanup();
}

// =================================================================
// Relative path handling
// =================================================================

// Relative paths: ./file and ../dir/file should be recorded with full path
void test_edge_relative_path() {
    cleanup();
    setup();
    run("mkdir -p /tmp/bdtrace_tp_work/sub");
    run("echo 'rel_data' > /tmp/bdtrace_tp_work/sub/rel.txt");

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    // cd into sub, then read ../sub/rel.txt and ./rel.txt
    argv.push_back("cd /tmp/bdtrace_tp_work/sub &&"
                    " cat ./rel.txt > /tmp/bdtrace_tp_work/rel_out1.txt &&"
                    " cat ../sub/rel.txt > /tmp/bdtrace_tp_work/rel_out2.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // Both should produce correct output
    std::string out1 = read_file("/tmp/bdtrace_tp_work/rel_out1.txt");
    std::string out2 = read_file("/tmp/bdtrace_tp_work/rel_out2.txt");
    ASSERT_STR_EQ(out1.c_str(), "rel_data\n");
    ASSERT_STR_EQ(out2.c_str(), "rel_data\n");

    // Verify file accesses are recorded (may be relative or absolute depending
    // on how the program opens them - either way they should exist)
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    bool found_rel = false;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("rel.txt") != std::string::npos
            && accesses[i].mode == FA_READ) {
            found_rel = true;
        }
    }
    ASSERT_TRUE(found_rel);

    cleanup();
}

// chdir should be recorded
void test_edge_chdir() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("cd /tmp/bdtrace_tp_work && pwd > /tmp/bdtrace_tp_work/pwd.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // pwd output should show the directory we cd'd to
    std::string content = read_file("/tmp/bdtrace_tp_work/pwd.txt");
    ASSERT_TRUE(content.find("bdtrace_tp_work") != std::string::npos);

    // Verify chdir is recorded in DB
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    bool found_chdir = false;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("bdtrace_tp_work") != std::string::npos
            && accesses[i].mode == FA_CHDIR) {
            found_chdir = true;
        }
    }
    ASSERT_TRUE(found_chdir);

    cleanup();
}

void run_transparency_tests() {
    std::printf("=== Transparency Tests ===\n");

    RUN_TEST(test_tp_exit_zero);
    RUN_TEST(test_tp_exit_one);
    RUN_TEST(test_tp_exit_arbitrary);
    RUN_TEST(test_tp_exit_conditional);
    RUN_TEST(test_tp_stdout_echo);
    RUN_TEST(test_tp_stdout_special_chars);
    RUN_TEST(test_tp_file_cp);
    RUN_TEST(test_tp_file_cat_concat);
    RUN_TEST(test_tp_file_sort);
    RUN_TEST(test_tp_pipe_passthrough);
    RUN_TEST(test_tp_pipe_multi_stage);
    RUN_TEST(test_tp_stderr);

    std::printf("=== Data Integrity Tests ===\n");

    RUN_TEST(test_integrity_timing);
    RUN_TEST(test_integrity_file_access_pids);
    RUN_TEST(test_integrity_parent_child);

    std::printf("=== Signal Transparency Tests ===\n");

    RUN_TEST(test_tp_signal_term);
    RUN_TEST(test_tp_signal_usr1);

    std::printf("=== Environment Variable Tests ===\n");

    RUN_TEST(test_tp_env_inherited);
    RUN_TEST(test_tp_env_path);

    std::printf("=== Edge Case Tests ===\n");

    RUN_TEST(test_edge_rapid_forks);
    RUN_TEST(test_edge_no_output);
    RUN_TEST(test_edge_deep_nesting);
    RUN_TEST(test_edge_append_mode);
    RUN_TEST(test_edge_empty_file);
    RUN_TEST(test_edge_background_procs);
    RUN_TEST(test_edge_many_files);
    RUN_TEST(test_edge_symlink);
    RUN_TEST(test_edge_hardlink);
    RUN_TEST(test_edge_relative_path);
    RUN_TEST(test_edge_chdir);
}
