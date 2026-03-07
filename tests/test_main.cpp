#include "helpers/test_helpers.h"
#include "../src/common/log.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_database_tests();
void run_trace_simple_tests();
void run_trace_fork_tests();
void run_trace_file_io_tests();
void run_rebuild_tests();

int main() {
    bdtrace::log_init(bdtrace::LOG_ERROR);

    run_database_tests();
    run_trace_simple_tests();
    run_trace_fork_tests();
    run_trace_file_io_tests();
    run_rebuild_tests();

    TEST_REPORT();
}
