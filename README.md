# bdtrace

A build tracer for analyzing `make` (or any command) builds on Linux. It follows the whole
process tree with ptrace, records every process and file access into a SQLite database,
and ships two analyzers: a CLI (`bdview`) and a web UI (`bdview-web`).

Designed to run on old systems: Linux 2.6.x (CentOS 5), GCC 4.1.2, C++03. No kernel
modules, no LD_PRELOAD, no seccomp required — plain ptrace, so it also works on any
modern kernel.

## What you get

- Full process tree with per-process duration, CPU time, peak RSS, and I/O bytes
- Every file open/stat/rename/unlink/... with read/write classification
- Failed accesses (with errno) — great for finding include-path misses
- Analysis commands: critical path, parallelism, hotspots, minimal rebuild sets,
  race detection, reverse dependencies, build diffing

## Requirements

- Linux 2.6.x or later, x86 or x86_64 (developed against CentOS 5 / kernel 2.6.18)
- GCC with C++03 support (GCC 4.1.2 works; modern GCC also works)
- `CAP_SYS_PTRACE` or default ptrace permissions (tracing your own children needs no privilege on most systems)

## Building

```sh
# One-time: fetch vendored third-party sources
bash scripts/fetch_sqlite.sh      # SQLite amalgamation -> vendor/
bash scripts/fetch_mongoose.sh    # Mongoose (only needed for bdview-web)

make            # builds bdtrace, bdview, bdview-web
make bdtrace    # or build individual binaries
DEBUG=1 make    # debug build (-g -O0)
make test       # build and run the unit test suite
```

## Quick start

```sh
./bdtrace -o build.db -- make -j8
./bdview summary build.db
./bdview critical build.db
./bdview-web build.db --port 8080     # then open http://127.0.0.1:8080
```

---

## bdtrace — the tracer

```
bdtrace [options] [--] command [args...]
```

Runs `command`, traces it and every descendant process, and writes the trace to a
SQLite database. Trace overhead depends on how syscall-heavy the build is; the
`--procs-only` mode is near-native speed.

| Option | Description |
|---|---|
| `-o FILE` | Output database file. Default: `bdtrace.db` |
| `--procs-only` | Record only the process tree (fork/exec/exit, durations, CPU/RSS/I/O). File accesses are not traced — tracees run under `PTRACE_CONT` at near-native speed. The exec'd binary of each process is still recorded. |
| `--no-seccomp` | Disable the seccomp-BPF fast path and always use classic full syscall tracing (see below). The `BDTRACE_NO_SECCOMP` environment variable does the same. |
| `--log-file F` | Also append WARN/ERROR messages to F. Default: `<output db>.log`; pass an empty string to disable. The file is only created if something is actually logged. |
| `-v` | Verbose output (debug logging) |
| `-h` | Show help |
| `--` | End of options; everything after is the command to trace |

### seccomp-BPF fast path

On kernels 3.5 and newer, bdtrace automatically installs a seccomp-BPF filter
in the traced command (inherited by all descendants) so that only the ~36
recorded syscalls stop the tracer; everything else (read/write/mmap/futex/...)
runs at native speed. This typically cuts full-trace overhead by 2–8× on
syscall-heavy builds. On Linux 2.6.x the fast path is skipped entirely and the
classic `PTRACE_SYSCALL` loop is used — behavior there is unchanged. The mode
in use is logged at startup (`seccomp-BPF fast path enabled`).

Caveat: the filter requires `no_new_privs`, so setuid binaries inside the
traced tree will not elevate privileges (they don't under plain ptrace
either). Use `--no-seccomp` if this — or anything else about the filter —
causes trouble.

Notes:

- Repeated identical accesses (same path + mode, per process incarnation) are
  deduplicated in memory and stored once.
- Accesses under `/dev/`, `/proc/`, and `/sys/` are filtered out.
- Kernel pid reuse (pids wrap at `pid_max`, 32768 on Linux 2.6, so long builds
  recycle them) is handled by remapping each reuse to a synthetic id:
  `generation × 10,000,000 + pid`. First uses keep the real pid, so short
  traces are unaffected; e.g. `10032417` in bdview means the second process
  that ran as pid 32417.
- `SIGINT`/`SIGTERM` are forwarded to the traced command and the trace is drained
  and finalized gracefully.
- Progress (processes, files, events/s, DB size) is printed to stderr every 60 s,
  plus diagnostic counters at exit.
- Indexes are built when the trace finishes; a database from an interrupted trace
  is repaired (indexes, counts) automatically the first time a viewer opens it.

## bdview — CLI analyzer

```
bdview <command> <database> [options]
```

The database is opened read-mostly; on first open, older databases are upgraded to
the current schema automatically.

| Command | Options | Description |
|---|---|---|
| `summary <db>` | — | Overall statistics: totals, max parallelism, slowest processes, CPU/RSS/I/O aggregates |
| `tree <db>` | `--slow MS` | Process tree with durations. `--slow` highlights processes slower than MS milliseconds |
| `slowest <db>` | `-n N` | Top N slowest processes. Default: 10 |
| `files <db>` | `-p PID`, `-f PATH` | File access info, filtered by process or by file path |
| `trace <db>` | `-p PID`, `-f PATH`, `-w N` | File accesses with full process ancestry chains. `-w` sets the column width |
| `rebuild <db>` | `--changed F` (required, repeatable), `--collapse CMD` (repeatable), `--estimate` | Minimal rebuild set for the given changed file(s). `--collapse` folds command names into their parents; `--estimate` prints a rebuild-time estimate |
| `timeline <db>` | `--min-duration MS` | Gantt chart of process execution; hides processes shorter than MS |
| `critical <db>` | — | Critical path analysis (the chain that bounds total build time) |
| `hotspot <db>` | `--top N`, `--dir` | Most-accessed files. Default top: 20. `--dir` aggregates by directory |
| `failures <db>` | `--errno N` | Failed file accesses, optionally filtered by errno value |
| `parallel <db>` | — | Parallelism over time; where the build is serial |
| `diff <db1> <db2>` | — | Compare two trace databases (takes two DB paths, no other options) |
| `diagnose <db>` | — | Auto-detect common build problems |
| `pio <db>` | `-p PID` (required), `--tree` | Process I/O summary (inputs/outputs). `--tree` includes the whole subtree of PID |
| `impact <db>` | `--top N` | Impact ranking of source files (how much rebuilds if each changes). Default top: 20 |
| `races <db>` | — | Detect parallel-build race conditions (concurrent write/read on the same file) |
| `rdeps <db>` | `-f PATH` (required), `--depth N` | Reverse file dependencies: what consumes PATH, transitively. Default depth: 3 |

## bdview-web — web UI

```
bdview-web <database.db> [options]
```

Serves an interactive viewer (process tree, timeline, file explorer) over HTTP.
The dependency graph is built once at startup.

| Option | Description |
|---|---|
| `--port PORT` | HTTP port. Default: `8080` |
| `--bind ADDR` | Bind address. Default: `127.0.0.1` (use `0.0.0.0` to expose on the network) |
| `--static-dir DIR` | Serve static assets from DIR instead of the embedded ones (development mode) |
| `-h`, `--help` | Show help |

---

## Database

Traces are plain SQLite files (WAL mode) — you can query them directly. Schema v6:

- `processes` — pid, ppid, cmdline, cwd, start/end time, exit code, CPU time, peak RSS, I/O bytes, access counts
- `files` — interned path strings (`id`, `path`)
- `file_accesses` — pid, `file_id`, mode, fd, timestamp
- `failed_accesses` — pid, `file_id`, mode, errno, timestamp
- `meta` — schema version etc.

Databases created by older versions (per-row filename text) are migrated in place
automatically when opened.

## Tests

```sh
make test                        # unit + integration suite (traces itself)
make test-lua-transparency       # optional: verify a real Lua build is unaffected
make test-gmake-transparency     # optional: same for GNU make
make test-openssl-transparency   # optional: same for OpenSSL
```

The transparency tests download and build real projects with and without tracing
and compare the results byte-for-byte (and report the timing overhead).

To run them in `--procs-only` mode — or with any other bdtrace options:

```sh
make test-lua-transparency-procs-only        # shorthand targets (also -gmake-, -openssl-)
make test-lua-transparency BDTRACE_ARGS=--procs-only   # equivalent, any options work
```
