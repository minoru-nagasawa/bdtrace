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

static const char* TEST_DB = "/tmp/bdtrace_test_fork.db";

static void cleanup() {
    unlink(TEST_DB);
}

void test_fork_children() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("/bin/true & /bin/true & wait");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);

    // Should have at least 3 processes: sh, and 2x /bin/true
    ASSERT_TRUE((int)procs.size() >= 3);

    // Verify parent-child relationships exist
    std::set<int> pids;
    for (size_t i = 0; i < procs.size(); ++i) {
        pids.insert(procs[i].pid);
    }

    // At least one process should have a ppid that's also in our set
    bool has_child = false;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (pids.find(procs[i].ppid) != pids.end()) {
            has_child = true;
            break;
        }
    }
    ASSERT_TRUE(has_child);

    cleanup();
}

void test_sequential_commands() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("/bin/true && /bin/echo hello > /dev/null");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);
    ASSERT_TRUE((int)procs.size() >= 2);

    cleanup();
}

int main() {
    log_init(LOG_ERROR);
    std::printf("=== Fork Trace Tests ===\n");

    RUN_TEST(test_fork_children);
    RUN_TEST(test_sequential_commands);

    TEST_REPORT();
}
