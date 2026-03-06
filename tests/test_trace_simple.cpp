#include "helpers/test_helpers.h"
#include "../src/common/types.h"
#include "../src/common/log.h"
#include "../src/tracer/trace_session.h"
#include "../src/tracer/ptrace_backend.h"

#include <unistd.h>
#include <string>
#include <vector>

using namespace bdtrace;

static const char* TEST_DB = "/tmp/bdtrace_test_simple.db";

static void cleanup() {
    unlink(TEST_DB);
}

void test_trace_true() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("/bin/true");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    Database& db = session.db();
    int count = db.get_process_count();
    ASSERT_TRUE(count >= 1);

    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);
    ASSERT_TRUE(!procs.empty());

    // The traced process should have /bin/true in cmdline
    bool found = false;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].cmdline.find("true") != std::string::npos) {
            found = true;
            ASSERT_EQ(procs[i].exit_code, 0);
        }
    }
    ASSERT_TRUE(found);

    cleanup();
}

void test_trace_false() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("/bin/false");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<ProcessRecord> procs;
    session.db().get_all_processes(procs);
    ASSERT_TRUE(!procs.empty());

    bool found = false;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].cmdline.find("false") != std::string::npos) {
            found = true;
            ASSERT_EQ(procs[i].exit_code, 1);
        }
    }
    ASSERT_TRUE(found);

    cleanup();
}

int main() {
    log_init(LOG_ERROR);
    std::printf("=== Simple Trace Tests ===\n");

    RUN_TEST(test_trace_true);
    RUN_TEST(test_trace_false);

    TEST_REPORT();
}
