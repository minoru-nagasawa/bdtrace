#include "helpers/test_helpers.h"
#include "../src/common/types.h"
#include "../src/common/log.h"
#include "../src/tracer/trace_session.h"
#include "../src/tracer/ptrace_backend.h"

#include <unistd.h>
#include <string>
#include <vector>
#include <set>
#include <cstdio>

using namespace bdtrace;

static const char* TEST_DB = "/tmp/bdtrace_test_shell_patterns.db";

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

static int trace_and_get_exit(const std::vector<std::string>& argv) {
    TraceSession session;
    if (!session.open(TEST_DB)) return -999;

    PtraceBackend backend(session);
    if (backend.start(argv) != 0) return -999;
    backend.run_event_loop();

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

// Returns path to bash, or NULL if not available
static const char* find_bash() {
    if (access("/bin/bash", X_OK) == 0) return "/bin/bash";
    if (access("/usr/bin/bash", X_OK) == 0) return "/usr/bin/bash";
    if (access("/usr/local/bin/bash", X_OK) == 0) return "/usr/local/bin/bash";
    return NULL;
}

// =================================================================
// Group 1: Redirection
// =================================================================

// stdin redirect: cat < input.txt > out.txt
void test_sp_stdin_redirect() {
    cleanup(); setup();
    run("echo 'stdin_data' > /tmp/bdtrace_tp_work/input.txt");

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("cat < /tmp/bdtrace_tp_work/input.txt > /tmp/bdtrace_tp_work/out.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/out.txt");
    ASSERT_STR_EQ(content.c_str(), "stdin_data\n");
    cleanup();
}

// stdout/stderr separation
void test_sp_stderr_stdout_merge() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sh -c 'echo out_msg; echo err_msg >&2'"
                    " >/tmp/bdtrace_tp_work/stdout.txt"
                    " 2>/tmp/bdtrace_tp_work/stderr.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string out = read_file("/tmp/bdtrace_tp_work/stdout.txt");
    std::string err = read_file("/tmp/bdtrace_tp_work/stderr.txt");
    ASSERT_STR_EQ(out.c_str(), "out_msg\n");
    ASSERT_STR_EQ(err.c_str(), "err_msg\n");
    cleanup();
}

// Redirect order matters: 2>&1 >a vs >b 2>&1
void test_sp_redirect_order_matters() {
    cleanup(); setup();

    // Case A: 2>&1 >a.txt  -- stderr goes to original stdout (lost), stdout goes to file
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sh -c 'echo OUT; echo ERR >&2' 2>&1 >/tmp/bdtrace_tp_work/a.txt");
    ASSERT_EQ(trace_and_get_exit(argv), 0);

    unlink(TEST_DB);
    // Case B: >b.txt 2>&1  -- both stdout and stderr go to file
    argv.clear();
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sh -c 'echo OUT; echo ERR >&2' >/tmp/bdtrace_tp_work/b.txt 2>&1");
    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string a = read_file("/tmp/bdtrace_tp_work/a.txt");
    std::string b = read_file("/tmp/bdtrace_tp_work/b.txt");
    // a.txt should have only stdout ("OUT\n")
    // b.txt should have both stdout and stderr
    ASSERT_STR_EQ(a.c_str(), "OUT\n");
    ASSERT_TRUE(b.find("OUT") != std::string::npos);
    ASSERT_TRUE(b.find("ERR") != std::string::npos);
    ASSERT_TRUE(a != b);
    cleanup();
}

// stderr to /dev/null + exit code
void test_sp_stderr_to_devnull() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("ls /tmp/bdtrace_nonexistent_xyz 2>/dev/null;"
                    " echo $? > /tmp/bdtrace_tp_work/rc.txt");
    ASSERT_EQ(trace_and_get_exit(argv), 0);

    std::string content = read_file("/tmp/bdtrace_tp_work/rc.txt");
    // ls on nonexistent should give non-zero exit code (typically 2)
    ASSERT_TRUE(content.size() > 0);
    ASSERT_TRUE(content[0] != '0');
    cleanup();
}

// bash &> redirect
void test_sp_ampersand_redirect() {
    const char* bash = find_bash();
    if (!bash) { std::printf("SKIP (no bash)\n"); return; }

    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back(bash);
    argv.push_back("-c");
    argv.push_back("bash -c 'echo out_msg; echo err_msg >&2' &>/tmp/bdtrace_tp_work/out.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/out.txt");
    ASSERT_TRUE(content.find("out_msg") != std::string::npos);
    ASSERT_TRUE(content.find("err_msg") != std::string::npos);
    cleanup();
}

// Multiple simultaneous redirects
void test_sp_multiple_redirects() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sh -c 'echo data_out; echo data_err >&2'"
                    " >/tmp/bdtrace_tp_work/stdout.txt"
                    " 2>/tmp/bdtrace_tp_work/stderr.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string out = read_file("/tmp/bdtrace_tp_work/stdout.txt");
    std::string err = read_file("/tmp/bdtrace_tp_work/stderr.txt");
    ASSERT_STR_EQ(out.c_str(), "data_out\n");
    ASSERT_STR_EQ(err.c_str(), "data_err\n");
    cleanup();
}

// Truncate redirect with no command
void test_sp_truncate_redirect() {
    cleanup(); setup();
    run("echo 'old_content' > /tmp/bdtrace_tp_work/trunc.txt");

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("> /tmp/bdtrace_tp_work/trunc.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/trunc.txt");
    ASSERT_STR_EQ(content.c_str(), "");
    cleanup();
}

// =================================================================
// Group 2: Pipes and tee
// =================================================================

// tee splits output to file and stdout
void test_sp_tee_split() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("echo tee_data | tee /tmp/bdtrace_tp_work/copy.txt"
                    " > /tmp/bdtrace_tp_work/out.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string copy = read_file("/tmp/bdtrace_tp_work/copy.txt");
    std::string out = read_file("/tmp/bdtrace_tp_work/out.txt");
    ASSERT_STR_EQ(copy.c_str(), "tee_data\n");
    ASSERT_STR_EQ(out.c_str(), "tee_data\n");
    cleanup();
}

// Pipe exit code: last command determines $?
void test_sp_pipe_exit_code() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("false | true; echo $? > /tmp/bdtrace_tp_work/rc.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/rc.txt");
    ASSERT_STR_EQ(content.c_str(), "0\n");
    cleanup();
}

// 5-stage pipe data integrity
void test_sp_long_pipe() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("printf 'cherry\\napple\\nbanana\\napple\\ncherry\\n'"
                    " | sort | uniq | tr 'a-z' 'A-Z' | head -2"
                    " > /tmp/bdtrace_tp_work/pipe5.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/pipe5.txt");
    ASSERT_STR_EQ(content.c_str(), "APPLE\nBANANA\n");
    cleanup();
}

// =================================================================
// Group 3: Conditional / sequential execution
// =================================================================

// Semicolon sequential execution
void test_sp_semicolon_sequential() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("echo one >> /tmp/bdtrace_tp_work/seq.txt;"
                    " echo two >> /tmp/bdtrace_tp_work/seq.txt;"
                    " echo three >> /tmp/bdtrace_tp_work/seq.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/seq.txt");
    ASSERT_STR_EQ(content.c_str(), "one\ntwo\nthree\n");
    cleanup();
}

// || operator fallback
void test_sp_or_operator() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("false || echo fallback > /tmp/bdtrace_tp_work/or.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/or.txt");
    ASSERT_STR_EQ(content.c_str(), "fallback\n");
    cleanup();
}

// =================================================================
// Group 4: Command substitution
// =================================================================

// $() command substitution
void test_sp_command_substitution() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("VAL=$(echo captured); echo $VAL > /tmp/bdtrace_tp_work/csub.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/csub.txt");
    ASSERT_STR_EQ(content.c_str(), "captured\n");
    cleanup();
}

// Nested command substitution
void test_sp_nested_command_substitution() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("echo $(echo inner_$(echo deep)) > /tmp/bdtrace_tp_work/nested.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/nested.txt");
    ASSERT_STR_EQ(content.c_str(), "inner_deep\n");
    cleanup();
}

// =================================================================
// Group 5: Subshell / grouping
// =================================================================

// Subshell output redirect
void test_sp_subshell() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("(echo sub1; echo sub2) > /tmp/bdtrace_tp_work/subshell.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/subshell.txt");
    ASSERT_STR_EQ(content.c_str(), "sub1\nsub2\n");
    cleanup();
}

// Brace group output redirect
void test_sp_brace_group() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("{ echo grp1; echo grp2; } > /tmp/bdtrace_tp_work/brace.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/brace.txt");
    ASSERT_STR_EQ(content.c_str(), "grp1\ngrp2\n");
    cleanup();
}

// =================================================================
// Group 6: Here document
// =================================================================

// Here document
void test_sp_heredoc() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("cat <<EOF > /tmp/bdtrace_tp_work/heredoc.txt\n"
                    "line1\n"
                    "line2\n"
                    "line3\n"
                    "EOF");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/heredoc.txt");
    ASSERT_STR_EQ(content.c_str(), "line1\nline2\nline3\n");
    cleanup();
}

// Here string (bash-specific)
void test_sp_here_string() {
    const char* bash = find_bash();
    if (!bash) { std::printf("SKIP (no bash)\n"); return; }

    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back(bash);
    argv.push_back("-c");
    argv.push_back("cat <<< 'here_string_data' > /tmp/bdtrace_tp_work/herestr.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/herestr.txt");
    ASSERT_STR_EQ(content.c_str(), "here_string_data\n");
    cleanup();
}

// =================================================================
// Group 7: FD operations
// =================================================================

// exec 3>file; echo >&3; exec 3>&-
void test_sp_fd_exec_redirect() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("exec 3>/tmp/bdtrace_tp_work/fd3.txt;"
                    " echo fd3_data >&3;"
                    " exec 3>&-");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/fd3.txt");
    ASSERT_STR_EQ(content.c_str(), "fd3_data\n");
    cleanup();
}

// stdout/stderr split via group redirect
void test_sp_fd_stdout_stderr_split() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("{ echo out_data; echo err_data >&2; }"
                    " >/tmp/bdtrace_tp_work/fd_out.txt"
                    " 2>/tmp/bdtrace_tp_work/fd_err.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string out = read_file("/tmp/bdtrace_tp_work/fd_out.txt");
    std::string err = read_file("/tmp/bdtrace_tp_work/fd_err.txt");
    ASSERT_STR_EQ(out.c_str(), "out_data\n");
    ASSERT_STR_EQ(err.c_str(), "err_data\n");
    cleanup();
}

// =================================================================
// Group 8: Indirect execution
// =================================================================

// xargs touch
void test_sp_xargs() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("printf '/tmp/bdtrace_tp_work/xa.txt\\n"
                    "/tmp/bdtrace_tp_work/xb.txt\\n"
                    "/tmp/bdtrace_tp_work/xc.txt\\n'"
                    " | xargs touch");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    ASSERT_EQ(access("/tmp/bdtrace_tp_work/xa.txt", F_OK), 0);
    ASSERT_EQ(access("/tmp/bdtrace_tp_work/xb.txt", F_OK), 0);
    ASSERT_EQ(access("/tmp/bdtrace_tp_work/xc.txt", F_OK), 0);
    cleanup();
}

// find -exec
void test_sp_find_exec() {
    cleanup(); setup();
    run("mkdir -p /tmp/bdtrace_tp_work/finddir");
    run("echo 'fa' > /tmp/bdtrace_tp_work/finddir/a.txt");
    run("echo 'fb' > /tmp/bdtrace_tp_work/finddir/b.txt");
    run("echo 'fc' > /tmp/bdtrace_tp_work/finddir/c.txt");

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("find /tmp/bdtrace_tp_work/finddir -name '*.txt' -exec cat {} \\;"
                    " | sort > /tmp/bdtrace_tp_work/find_out.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/find_out.txt");
    ASSERT_STR_EQ(content.c_str(), "fa\nfb\nfc\n");
    cleanup();
}

// Nested sh -c
void test_sp_sh_c_complex() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("sh -c \"echo inner1; sh -c 'echo inner2'\""
                    " > /tmp/bdtrace_tp_work/shc.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/shc.txt");
    ASSERT_STR_EQ(content.c_str(), "inner1\ninner2\n");
    cleanup();
}

// =================================================================
// Group 9: Environment variable prefix
// =================================================================

void test_sp_env_prefix() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("MY_VAR=test_val sh -c 'echo $MY_VAR > /tmp/bdtrace_tp_work/envp.txt'");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/envp.txt");
    ASSERT_STR_EQ(content.c_str(), "test_val\n");
    cleanup();
}

// =================================================================
// Group 10: Wildcard / glob
// =================================================================

void test_sp_glob_wildcard() {
    cleanup(); setup();
    run("touch /tmp/bdtrace_tp_work/glob_a.txt");
    run("touch /tmp/bdtrace_tp_work/glob_b.txt");
    run("touch /tmp/bdtrace_tp_work/glob_c.txt");

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("ls /tmp/bdtrace_tp_work/glob_*.txt | sort"
                    " > /tmp/bdtrace_tp_work/glob_out.txt");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/glob_out.txt");
    ASSERT_TRUE(content.find("glob_a.txt") != std::string::npos);
    ASSERT_TRUE(content.find("glob_b.txt") != std::string::npos);
    ASSERT_TRUE(content.find("glob_c.txt") != std::string::npos);
    cleanup();
}

// =================================================================
// Group 11: Named pipe (FIFO)
// =================================================================

void test_sp_named_pipe() {
    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("mkfifo /tmp/bdtrace_tp_work/fifo;"
                    " echo fifo_data > /tmp/bdtrace_tp_work/fifo &"
                    " cat /tmp/bdtrace_tp_work/fifo > /tmp/bdtrace_tp_work/fifo_out.txt;"
                    " wait");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    std::string content = read_file("/tmp/bdtrace_tp_work/fifo_out.txt");
    ASSERT_STR_EQ(content.c_str(), "fifo_data\n");
    cleanup();
}

// =================================================================
// Group 12: Process substitution (bash-specific)
// =================================================================

void test_sp_process_substitution() {
    const char* bash = find_bash();
    if (!bash) { std::printf("SKIP (no bash)\n"); return; }

    cleanup(); setup();

    std::vector<std::string> argv;
    argv.push_back(bash);
    argv.push_back("-c");
    argv.push_back("diff <(echo same_data) <(echo same_data)"
                    " > /tmp/bdtrace_tp_work/psub.txt; true");

    ASSERT_EQ(trace_and_get_exit(argv), 0);
    // diff of identical inputs should produce empty output
    std::string content = read_file("/tmp/bdtrace_tp_work/psub.txt");
    ASSERT_STR_EQ(content.c_str(), "");
    cleanup();
}

// =================================================================
// Group 13: Parallel execution
// =================================================================

void test_sp_parallel_xargs() {
    cleanup(); setup();
    run("mkdir -p /tmp/bdtrace_tp_work/psrc /tmp/bdtrace_tp_work/pdst");
    for (int i = 0; i < 10; ++i) {
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd),
                      "echo 'data%d' > /tmp/bdtrace_tp_work/psrc/f%02d.txt", i, i);
        run(cmd);
    }

    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("ls /tmp/bdtrace_tp_work/psrc/*.txt"
                    " | xargs -P 4 -I{} cp {} /tmp/bdtrace_tp_work/pdst/");

    ASSERT_EQ(trace_and_get_exit(argv), 0);

    // Verify all 10 files were copied
    for (int i = 0; i < 10; ++i) {
        char path[256];
        std::snprintf(path, sizeof(path),
                      "/tmp/bdtrace_tp_work/pdst/f%02d.txt", i);
        ASSERT_EQ(access(path, F_OK), 0);

        char expected[32];
        std::snprintf(expected, sizeof(expected), "data%d\n", i);
        std::string content = read_file(path);
        ASSERT_STR_EQ(content.c_str(), expected);
    }
    cleanup();
}

// =================================================================
// Entry point
// =================================================================

void run_shell_pattern_tests() {
    std::printf("=== Shell Pattern Tests (Redirection) ===\n");
    RUN_TEST(test_sp_stdin_redirect);
    RUN_TEST(test_sp_stderr_stdout_merge);
    RUN_TEST(test_sp_redirect_order_matters);
    RUN_TEST(test_sp_stderr_to_devnull);
    RUN_TEST(test_sp_multiple_redirects);
    RUN_TEST(test_sp_truncate_redirect);

    std::printf("=== Shell Pattern Tests (Pipes) ===\n");
    RUN_TEST(test_sp_tee_split);
    RUN_TEST(test_sp_pipe_exit_code);
    RUN_TEST(test_sp_long_pipe);

    std::printf("=== Shell Pattern Tests (Conditional/Sequential) ===\n");
    RUN_TEST(test_sp_semicolon_sequential);
    RUN_TEST(test_sp_or_operator);

    std::printf("=== Shell Pattern Tests (Command Substitution) ===\n");
    RUN_TEST(test_sp_command_substitution);
    RUN_TEST(test_sp_nested_command_substitution);

    std::printf("=== Shell Pattern Tests (Subshell/Grouping) ===\n");
    RUN_TEST(test_sp_subshell);
    RUN_TEST(test_sp_brace_group);

    std::printf("=== Shell Pattern Tests (Here Document) ===\n");
    RUN_TEST(test_sp_heredoc);

    std::printf("=== Shell Pattern Tests (FD Operations) ===\n");
    RUN_TEST(test_sp_fd_exec_redirect);
    RUN_TEST(test_sp_fd_stdout_stderr_split);

    std::printf("=== Shell Pattern Tests (Indirect Execution) ===\n");
    RUN_TEST(test_sp_xargs);
    RUN_TEST(test_sp_find_exec);
    RUN_TEST(test_sp_sh_c_complex);

    std::printf("=== Shell Pattern Tests (Environment) ===\n");
    RUN_TEST(test_sp_env_prefix);

    std::printf("=== Shell Pattern Tests (Wildcard) ===\n");
    RUN_TEST(test_sp_glob_wildcard);

    std::printf("=== Shell Pattern Tests (Named Pipe) ===\n");
    RUN_TEST(test_sp_named_pipe);

    std::printf("=== Shell Pattern Tests (Parallel) ===\n");
    RUN_TEST(test_sp_parallel_xargs);

    std::printf("=== Shell Pattern Tests (Bash-specific) ===\n");
    RUN_TEST(test_sp_ampersand_redirect);
    RUN_TEST(test_sp_here_string);
    RUN_TEST(test_sp_process_substitution);
}
