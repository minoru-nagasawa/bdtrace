#include "helpers/test_helpers.h"
#include "../src/common/log.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_database_tests();
void run_trace_simple_tests();
void run_trace_fork_tests();
void run_trace_file_io_tests();
void run_rebuild_tests();
void run_transparency_tests();
void run_shell_pattern_tests();
void run_analysis_tests();
void run_cmd_name_tests();

int main() {
    bdtrace::log_init(bdtrace::LOG_ERROR);

    run_database_tests();
    run_trace_simple_tests();
    run_trace_fork_tests();
    run_trace_file_io_tests();
    run_rebuild_tests();
    run_transparency_tests();
    run_shell_pattern_tests();
    run_analysis_tests();
    run_cmd_name_tests();

    TEST_REPORT();
}
