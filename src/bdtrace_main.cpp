#include "common/types.h"
#include "common/log.h"
#include "tracer/trace_session.h"
#include "tracer/ptrace_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void usage() {
    std::fprintf(stderr,
        "Usage: bdtrace [options] command [args...]\n"
        "\n"
        "Options:\n"
        "  -o FILE   Output database file (default: bdtrace.db)\n"
        "  -v        Verbose output (debug logging)\n"
        "  -h        Show this help\n"
    );
}

int main(int argc, char* argv[]) {
    std::string db_path = "bdtrace.db";
    bdtrace::LogLevel log_level = bdtrace::LOG_INFO;

    int cmd_start = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            db_path = argv[++i];
            cmd_start = i + 1;
        } else if (std::strcmp(argv[i], "-v") == 0) {
            log_level = bdtrace::LOG_DEBUG;
            cmd_start = i + 1;
        } else if (std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            cmd_start = i;
            break;
        }
    }

    if (cmd_start >= argc) {
        usage();
        return 1;
    }

    bdtrace::log_init(log_level);

    std::vector<std::string> cmd_argv;
    for (int i = cmd_start; i < argc; ++i) {
        cmd_argv.push_back(argv[i]);
    }

    bdtrace::TraceSession session;
    if (!session.open(db_path)) {
        std::fprintf(stderr, "bdtrace: failed to open database: %s\n", db_path.c_str());
        return 1;
    }

    bdtrace::PtraceBackend backend(session);
    if (backend.start(cmd_argv) != 0) {
        std::fprintf(stderr, "bdtrace: failed to start tracing\n");
        return 1;
    }

    int rc = backend.run_event_loop();

    LOG_INFO("Trace complete. Database: %s", db_path.c_str());
    return rc;
}
