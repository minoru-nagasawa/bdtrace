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
    argv.push_back("cat /etc/hosts > /tmp/bdtrace_tp_work/host.txt");

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

// =================================================================
// Timing ratio preservation: bdtrace overhead should be uniform
// =================================================================

// Measure wall-clock time for running a command via system()
static int64_t time_system(const char* cmd) {
    int64_t t0 = now_us();
    int ret = system(cmd);
    (void)ret;
    int64_t t1 = now_us();
    return t1 - t0;
}

// Verify that task duration ratios don't change significantly under tracing.
// Run a short and long task both with and without tracing, compare ratios.
void test_tp_timing_ratio() {
    cleanup();
    setup();

    // Generate test data using awk (available on CentOS 5, unlike shuf)
    run("awk 'BEGIN{for(i=1;i<=100;i++)print i}' > /tmp/bdtrace_tp_work/short.txt");
    run("awk 'BEGIN{for(i=1;i<=10000;i++)print i}' > /tmp/bdtrace_tp_work/long.txt");

    // Warm up filesystem cache
    run("sort /tmp/bdtrace_tp_work/short.txt > /dev/null");
    run("sort /tmp/bdtrace_tp_work/long.txt > /dev/null");

    int64_t base_short = time_system("sort /tmp/bdtrace_tp_work/short.txt > /dev/null");
    int64_t base_long = time_system("sort /tmp/bdtrace_tp_work/long.txt > /dev/null");

    // Traced: same tasks
    {
        TraceSession session;
        ASSERT_TRUE(session.open(TEST_DB));
        PtraceBackend backend(session);
        std::vector<std::string> argv;
        argv.push_back("sort");
        argv.push_back("/tmp/bdtrace_tp_work/short.txt");
        argv.push_back("-o");
        argv.push_back("/tmp/bdtrace_tp_work/short_sorted.txt");
        int64_t t0 = now_us();
        ASSERT_EQ(backend.start(argv), 0);
        backend.run_event_loop();
        int64_t traced_short = now_us() - t0;

        cleanup();
        setup();
        run("awk 'BEGIN{for(i=1;i<=100;i++)print i}' > /tmp/bdtrace_tp_work/short.txt");
        run("awk 'BEGIN{for(i=1;i<=10000;i++)print i}' > /tmp/bdtrace_tp_work/long.txt");

        TraceSession session2;
        ASSERT_TRUE(session2.open(TEST_DB));
        PtraceBackend backend2(session2);
        std::vector<std::string> argv2;
        argv2.push_back("sort");
        argv2.push_back("/tmp/bdtrace_tp_work/long.txt");
        argv2.push_back("-o");
        argv2.push_back("/tmp/bdtrace_tp_work/long_sorted.txt");
        t0 = now_us();
        ASSERT_EQ(backend2.start(argv2), 0);
        backend2.run_event_loop();
        int64_t traced_long = now_us() - t0;

        // The ratio of long/short should be similar between baseline and traced.
        // Allow generous tolerance (5x) since short tasks have more overhead noise.
        if (base_short > 0 && traced_short > 0) {
            double base_ratio = (double)base_long / (double)base_short;
            double traced_ratio = (double)traced_long / (double)traced_short;

            // The ratios should be in the same order of magnitude
            if (base_ratio > 1.0) {
                ASSERT_TRUE(traced_ratio > base_ratio / 5.0);
                ASSERT_TRUE(traced_ratio < base_ratio * 5.0);
            }
        }
    }

    cleanup();
}

// =================================================================
// Build simulation: multi-step pipeline correctness
// =================================================================

void test_tp_build_simulation() {
    cleanup();
    setup();
    run("mkdir -p /tmp/bdtrace_tp_work/build");

    // Write source files
    run("echo 'int x = 1;' > /tmp/bdtrace_tp_work/build/a.c");
    run("echo 'int y = 2;' > /tmp/bdtrace_tp_work/build/b.c");

    // Trace a multi-step build pipeline:
    // 1. Preprocess: cat files together
    // 2. Transform: sort the combined output
    // 3. Package: wc -l on the result
    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));
    PtraceBackend backend(session);

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back(
        "cat /tmp/bdtrace_tp_work/build/a.c /tmp/bdtrace_tp_work/build/b.c"
        " > /tmp/bdtrace_tp_work/build/combined.txt &&"
        " sort /tmp/bdtrace_tp_work/build/combined.txt"
        " > /tmp/bdtrace_tp_work/build/sorted.txt &&"
        " wc -l < /tmp/bdtrace_tp_work/build/sorted.txt"
        " > /tmp/bdtrace_tp_work/build/count.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // Verify pipeline outputs
    std::string combined = read_file("/tmp/bdtrace_tp_work/build/combined.txt");
    ASSERT_STR_EQ(combined.c_str(), "int x = 1;\nint y = 2;\n");

    std::string sorted = read_file("/tmp/bdtrace_tp_work/build/sorted.txt");
    ASSERT_STR_EQ(sorted.c_str(), "int x = 1;\nint y = 2;\n");

    std::string count = read_file("/tmp/bdtrace_tp_work/build/count.txt");
    // wc -l output may have leading spaces
    ASSERT_TRUE(count.find("2") != std::string::npos);

    // Verify DB recorded the file accesses
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    bool found_read_a = false, found_read_b = false;
    bool found_write_combined = false, found_write_sorted = false;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("a.c") != std::string::npos
            && accesses[i].mode == FA_READ) found_read_a = true;
        if (accesses[i].filename.find("b.c") != std::string::npos
            && accesses[i].mode == FA_READ) found_read_b = true;
        if (accesses[i].filename.find("combined.txt") != std::string::npos
            && (accesses[i].mode == FA_WRITE || accesses[i].mode == FA_RDWR))
            found_write_combined = true;
        if (accesses[i].filename.find("sorted.txt") != std::string::npos
            && (accesses[i].mode == FA_WRITE || accesses[i].mode == FA_RDWR))
            found_write_sorted = true;
    }
    ASSERT_TRUE(found_read_a);
    ASSERT_TRUE(found_read_b);
    ASSERT_TRUE(found_write_combined);
    ASSERT_TRUE(found_write_sorted);

    cleanup();
}

// =================================================================
// Resource tracking consistency
// =================================================================

void test_integrity_resources() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    // Do some actual work so resource stats are non-zero
    argv.push_back("awk 'BEGIN{for(i=1;i<=10000;i++)print i}' | sort > /tmp/bdtrace_tp_work/res_out.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);

    for (size_t i = 0; i < procs.size(); ++i) {
        // Resource values should be non-negative
        ASSERT_TRUE(procs[i].user_time_us >= 0);
        ASSERT_TRUE(procs[i].sys_time_us >= 0);
        ASSERT_TRUE(procs[i].peak_rss_kb >= 0);
        ASSERT_TRUE(procs[i].io_read_bytes >= 0);
        ASSERT_TRUE(procs[i].io_write_bytes >= 0);
    }

    // At least one process should have non-zero CPU time or RSS
    // (CPU time granularity is 10ms on Linux 2.6, so very fast tasks may report 0)
    bool any_resource = false;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].user_time_us > 0 || procs[i].sys_time_us > 0
            || procs[i].peak_rss_kb > 0) {
            any_resource = true;
            break;
        }
    }
    ASSERT_TRUE(any_resource);

    cleanup();
}

// =================================================================
// Parallel processes with shared files
// =================================================================

void test_tp_parallel_shared_files() {
    cleanup();
    setup();

    run("echo 'shared_data' > /tmp/bdtrace_tp_work/shared_input.txt");

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));
    PtraceBackend backend(session);

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    // Three parallel processes all read the same file
    argv.push_back(
        "cat /tmp/bdtrace_tp_work/shared_input.txt > /tmp/bdtrace_tp_work/out1.txt &"
        " cat /tmp/bdtrace_tp_work/shared_input.txt > /tmp/bdtrace_tp_work/out2.txt &"
        " cat /tmp/bdtrace_tp_work/shared_input.txt > /tmp/bdtrace_tp_work/out3.txt &"
        " wait");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    // All outputs should be identical to input
    ASSERT_STR_EQ(read_file("/tmp/bdtrace_tp_work/out1.txt").c_str(), "shared_data\n");
    ASSERT_STR_EQ(read_file("/tmp/bdtrace_tp_work/out2.txt").c_str(), "shared_data\n");
    ASSERT_STR_EQ(read_file("/tmp/bdtrace_tp_work/out3.txt").c_str(), "shared_data\n");

    // DB should record reads from shared file by multiple PIDs
    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);
    std::set<int> shared_readers;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("shared_input.txt") != std::string::npos
            && accesses[i].mode == FA_READ) {
            shared_readers.insert(accesses[i].pid);
        }
    }
    ASSERT_TRUE((int)shared_readers.size() >= 3);

    cleanup();
}

// =================================================================
// Timing: children enclosed in parent time range
// =================================================================

void test_integrity_timing_parent_child() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));
    PtraceBackend backend(session);

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sh -c '/bin/true' && sh -c '/bin/true'");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);

    // Build parent map
    std::map<int, ProcessRecord> pmap;
    for (size_t i = 0; i < procs.size(); ++i) {
        pmap[procs[i].pid] = procs[i];
    }

    // Every child's time range should be within its parent's range
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].end_time_us <= 0) continue;
        std::map<int, ProcessRecord>::const_iterator parent = pmap.find(procs[i].ppid);
        if (parent == pmap.end()) continue;  // root process
        if (parent->second.end_time_us <= 0) continue;

        // Child should start at or after parent starts (allow 1ms tolerance for ptrace event ordering)
        ASSERT_TRUE(procs[i].start_time_us >= parent->second.start_time_us - 1000);
        // Child should end at or before parent ends (allow 1ms tolerance for ptrace event ordering)
        ASSERT_TRUE(procs[i].end_time_us <= parent->second.end_time_us + 1000);
    }

    cleanup();
}

// =================================================================
// File count denormalization after trace
// =================================================================

void test_integrity_file_counts() {
    cleanup();
    setup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));
    PtraceBackend backend(session);

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("cat /etc/hosts > /tmp/bdtrace_tp_work/fcnt.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();
    session.finalize();

    // After finalize, file_count should be populated
    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);

    // At least one process should have non-zero file_count
    bool any_files = false;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].file_count > 0) {
            any_files = true;
            // Verify it matches actual count
            std::vector<FileAccessRecord> fa;
            session.db().get_file_accesses_by_pid(procs[i].pid, fa);
            ASSERT_EQ(procs[i].file_count, (int)fa.size());
        }
    }
    ASSERT_TRUE(any_files);

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
    RUN_TEST(test_integrity_resources);
    RUN_TEST(test_integrity_timing_parent_child);
    RUN_TEST(test_integrity_file_counts);

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

    std::printf("=== Build Simulation Tests ===\n");

    RUN_TEST(test_tp_timing_ratio);
    RUN_TEST(test_tp_build_simulation);
    RUN_TEST(test_tp_parallel_shared_files);
}
