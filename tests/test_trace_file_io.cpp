#include "helpers/test_helpers.h"
#include "../src/common/types.h"
#include "../src/common/log.h"
#include "../src/tracer/trace_session.h"
#include "../src/tracer/ptrace_backend.h"

#include <unistd.h>
#include <string>
#include <vector>

using namespace bdtrace;

static const char* TEST_DB = "/tmp/bdtrace_test_fileio.db";

static void cleanup() {
    unlink(TEST_DB);
    unlink("/tmp/bdtrace_test_out.txt");
}

void test_file_read() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("cat");
    argv.push_back("/etc/hostname");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);

    // Should have at least one file read for /etc/hostname
    bool found_read = false;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("hostname") != std::string::npos) {
            found_read = true;
            ASSERT_EQ(accesses[i].mode, FA_READ);
        }
    }
    ASSERT_TRUE(found_read);

    cleanup();
}

void test_file_write() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("echo hello > /tmp/bdtrace_test_out.txt");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);

    bool found_write = false;
    for (size_t i = 0; i < accesses.size(); ++i) {
        if (accesses[i].filename.find("bdtrace_test_out") != std::string::npos) {
            found_write = true;
            ASSERT_TRUE(accesses[i].mode == FA_WRITE || accesses[i].mode == FA_RDWR);
        }
    }
    ASSERT_TRUE(found_write);

    cleanup();
}

void test_filtered_paths() {
    cleanup();

    TraceSession session;
    ASSERT_TRUE(session.open(TEST_DB));

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("ls");
    argv.push_back("/dev/null");

    ASSERT_EQ(backend.start(argv), 0);
    backend.run_event_loop();

    std::vector<FileAccessRecord> accesses;
    session.db().get_all_file_accesses(accesses);

    // /dev/ paths should be filtered out
    for (size_t i = 0; i < accesses.size(); ++i) {
        ASSERT_TRUE(accesses[i].filename.compare(0, 5, "/dev/") != 0);
        ASSERT_TRUE(accesses[i].filename.compare(0, 6, "/proc/") != 0);
        ASSERT_TRUE(accesses[i].filename.compare(0, 4, "/sys/") != 0);
    }

    cleanup();
}

void run_trace_file_io_tests() {
    std::printf("=== File I/O Trace Tests ===\n");

    RUN_TEST(test_file_read);
    RUN_TEST(test_file_write);
    RUN_TEST(test_filtered_paths);
}
