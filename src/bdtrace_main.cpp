#include "common/types.h"
#include "common/log.h"
#include "tracer/trace_session.h"
#include "tracer/ptrace_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <signal.h>

static void usage() {
    std::fprintf(stderr,
        "Usage: bdtrace [options] command [args...]\n"
        "\n"
        "Options:\n"
        "  -o FILE       Output database file (default: bdtrace.db)\n"
        "  --procs-only  Record only the process tree (no file accesses);\n"
        "                tracees run at near-native speed\n"
        "  --no-seccomp  Disable the seccomp-BPF fast path (kernel >= 3.5);\n"
        "                always use classic full syscall tracing\n"
        "  -v            Verbose output (debug logging)\n"
        "  -h            Show this help\n"
        "  --            End of options; the command follows\n"
    );
}

int main(int argc, char* argv[]) {
    std::string db_path = "bdtrace.db";
    bdtrace::LogLevel log_level = bdtrace::LOG_INFO;
    bool procs_only = false;
    bool no_seccomp = false;

    int cmd_start = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            db_path = argv[++i];
            cmd_start = i + 1;
        } else if (std::strcmp(argv[i], "--procs-only") == 0) {
            procs_only = true;
            cmd_start = i + 1;
        } else if (std::strcmp(argv[i], "--no-seccomp") == 0) {
            no_seccomp = true;
            cmd_start = i + 1;
        } else if (std::strcmp(argv[i], "-v") == 0) {
            log_level = bdtrace::LOG_DEBUG;
            cmd_start = i + 1;
        } else if (std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (std::strcmp(argv[i], "--") == 0) {
            cmd_start = i + 1;
            break;
        } else {
            cmd_start = i;
            break;
        }
    }

    if (cmd_start >= argc) {
        usage();
        return 1;
    }

    // P1.4: Ignore SIGPIPE to prevent crash when stderr pipe breaks
    signal(SIGPIPE, SIG_IGN);

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
    backend.set_procs_only(procs_only);
    backend.set_seccomp_allowed(!no_seccomp);
    if (backend.start(cmd_argv) != 0) {
        std::fprintf(stderr, "bdtrace: failed to start tracing\n");
        return 1;
    }

    int rc = backend.run_event_loop();

    LOG_INFO("Trace complete. Database: %s", db_path.c_str());
    return rc;
}
