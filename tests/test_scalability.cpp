#include "helpers/test_helpers.h"
#include "../src/common/types.h"
#include "../src/common/log.h"
#include "../src/db/database.h"

#include <unistd.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <cstdio>

using namespace bdtrace;

static const char* TEST_DB = "/tmp/bdtrace_test_scale.db";

static void cleanup() {
    unlink(TEST_DB);
    std::string shm = std::string(TEST_DB) + "-shm";
    std::string wal = std::string(TEST_DB) + "-wal";
    unlink(shm.c_str());
    unlink(wal.c_str());
}

// Populate a large database for subsequent tests.
// 5000 processes (tree structure), 50000 file accesses, 5000 failed accesses
static bool populate_large_db(Database& db) {
    if (!db.begin_transaction()) return false;

    // Create processes: root -> 50 groups of 100 children
    ProcessRecord root;
    root.pid = 1;
    root.ppid = 0;
    root.cmdline = "make -j50";
    root.cwd = "/build";
    root.start_time_us = 1000000;
    root.end_time_us = 100000000;
    root.exit_code = 0;
    if (!db.insert_process(root)) return false;

    int pid_counter = 2;
    for (int group = 0; group < 50; ++group) {
        // Group parent (e.g., gcc wrapper)
        ProcessRecord gp;
        gp.pid = pid_counter++;
        gp.ppid = 1;
        char cmd[128];
        std::snprintf(cmd, sizeof(cmd), "sh -c 'gcc -c file%d.c'", group);
        gp.cmdline = cmd;
        gp.cwd = "/build";
        gp.start_time_us = 1000000 + group * 2000000;
        gp.end_time_us = gp.start_time_us + 1900000;
        gp.exit_code = 0;
        gp.user_time_us = 500000;
        gp.sys_time_us = 100000;
        gp.peak_rss_kb = 50000;
        gp.io_read_bytes = 1000000;
        gp.io_write_bytes = 500000;
        if (!db.insert_process(gp)) return false;

        // Children under each group (99 each -> 50*99=4950 + 50 parents + 1 root = 5001)
        int group_parent = gp.pid;
        for (int child = 0; child < 99; ++child) {
            ProcessRecord cp;
            cp.pid = pid_counter++;
            cp.ppid = group_parent;
            std::snprintf(cmd, sizeof(cmd), "cc1 file%d_%d.c", group, child);
            cp.cmdline = cmd;
            cp.cwd = "/build";
            cp.start_time_us = gp.start_time_us + child * 18000;
            cp.end_time_us = cp.start_time_us + 17000;
            cp.exit_code = (child == 50 && group == 25) ? 1 : 0;  // one failure
            cp.user_time_us = 10000;
            cp.sys_time_us = 2000;
            cp.peak_rss_kb = 20000;
            if (!db.insert_process(cp)) return false;
        }
    }

    // Create file accesses: 10 files per process (pid_counter-1 = total processes)
    int total_procs = pid_counter - 1;
    int fa_count = 0;
    for (int pid = 1; pid <= total_procs && fa_count < 50000; ++pid) {
        for (int f = 0; f < 10 && fa_count < 50000; ++f) {
            FileAccessRecord fa;
            fa.pid = pid;
            char fname[128];
            // Mix of different directories
            int dir = (pid * 7 + f) % 20;
            std::snprintf(fname, sizeof(fname), "/build/src/dir%d/file%d_%d.c", dir, pid, f);
            fa.filename = fname;
            fa.mode = static_cast<FileAccessMode>(f % 3);  // READ, WRITE, RDWR
            fa.fd = 3 + f;
            fa.timestamp_us = 1000000 + pid * 100 + f;
            if (!db.insert_file_access(fa)) return false;
            ++fa_count;
        }
    }

    // Add some shared files (hotfiles) — many processes read the same header
    for (int pid = 1; pid <= 2000; ++pid) {
        FileAccessRecord fa;
        fa.pid = pid;
        fa.filename = "/usr/include/stdio.h";
        fa.mode = FA_READ;
        fa.fd = 3;
        fa.timestamp_us = 1000000 + pid;
        if (!db.insert_file_access(fa)) return false;
    }
    for (int pid = 1; pid <= 500; ++pid) {
        FileAccessRecord fa;
        fa.pid = pid;
        fa.filename = "/build/include/common.h";
        fa.mode = FA_READ;
        fa.fd = 3;
        fa.timestamp_us = 1000000 + pid;
        if (!db.insert_file_access(fa)) return false;
    }

    // Create failed accesses
    for (int i = 0; i < 5000; ++i) {
        FailedAccessRecord fr;
        fr.pid = 1 + (i % 1000);
        char fname[128];
        std::snprintf(fname, sizeof(fname), "/build/missing/file%d.h", i);
        fr.filename = fname;
        fr.mode = FA_READ;
        fr.errno_val = 2;  // ENOENT
        fr.timestamp_us = 1000000 + i;
        if (!db.insert_failed_access(fr)) return false;
    }

    if (!db.commit_transaction()) return false;
    return true;
}

// ==================================================================
// Test 1: Large-scale insertion performance
// ==================================================================
void test_scale_insert() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    int64_t t0 = now_us();
    ASSERT_TRUE(populate_large_db(db));
    int64_t t1 = now_us();

    // Verify counts
    ASSERT_TRUE(db.get_process_count() >= 5000);
    ASSERT_TRUE(db.get_file_access_count() >= 50000);
    ASSERT_TRUE(db.get_failed_access_count() >= 5000);

    // Insertion should complete within 10 seconds
    int64_t elapsed_us = t1 - t0;
    ASSERT_TRUE(elapsed_us < 10000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 2: Query all processes (ordered)
// ==================================================================
void test_scale_query_all_processes() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    int64_t t0 = now_us();
    std::vector<ProcessRecord> procs;
    ASSERT_TRUE(db.get_all_processes(procs));
    int64_t t1 = now_us();

    ASSERT_TRUE((int)procs.size() >= 5000);
    // Results should be ordered by start_time_us
    for (size_t i = 1; i < procs.size(); ++i) {
        ASSERT_TRUE(procs[i].start_time_us >= procs[i-1].start_time_us);
    }
    // Should complete within 2 seconds
    ASSERT_TRUE(t1 - t0 < 2000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 3: File access by PID (index lookup)
// ==================================================================
void test_scale_query_fa_by_pid() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    // Query for a specific PID should be fast
    int64_t t0 = now_us();
    for (int pid = 1; pid <= 100; ++pid) {
        std::vector<FileAccessRecord> fa;
        ASSERT_TRUE(db.get_file_accesses_by_pid(pid, fa));
        ASSERT_TRUE(!fa.empty());
    }
    int64_t t1 = now_us();
    // 100 indexed lookups should complete within 1 second
    ASSERT_TRUE(t1 - t0 < 1000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 4: Prefix range query
// ==================================================================
void test_scale_query_fa_by_prefix() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    int64_t t0 = now_us();
    std::vector<FileAccessRecord> fa;
    ASSERT_TRUE(db.get_file_accesses_by_prefix("/build/src/dir0/", fa));
    int64_t t1 = now_us();

    ASSERT_TRUE(!fa.empty());
    // All results should start with the prefix
    for (size_t i = 0; i < fa.size(); ++i) {
        ASSERT_TRUE(fa[i].filename.find("/build/src/dir0/") == 0);
    }
    // Prefix query should be fast (uses index)
    ASSERT_TRUE(t1 - t0 < 500000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 5: populate_counts
// ==================================================================
void test_scale_populate_counts() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    int64_t t0 = now_us();
    ASSERT_TRUE(db.populate_counts());
    int64_t t1 = now_us();

    // Verify counts were set
    ProcessRecord proc;
    ASSERT_TRUE(db.get_process(1, proc));
    ASSERT_TRUE(proc.file_count > 0);

    // Should complete within 5 seconds
    ASSERT_TRUE(t1 - t0 < 5000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 6: File stats grouped (SQL GROUP BY)
// ==================================================================
void test_scale_file_stats_grouped() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    int64_t t0 = now_us();
    std::vector<FileStatRow> stats;
    ASSERT_TRUE(db.get_file_stats_grouped(stats));
    int64_t t1 = now_us();

    // Should have many unique filenames
    ASSERT_TRUE((int)stats.size() > 1000);

    // Verify hotfiles show up
    bool found_stdio = false;
    for (size_t i = 0; i < stats.size(); ++i) {
        if (stats[i].filename == "/usr/include/stdio.h") {
            found_stdio = true;
            ASSERT_TRUE(stats[i].process_count >= 2000);
            ASSERT_TRUE(stats[i].read_count >= 2000);
        }
    }
    ASSERT_TRUE(found_stdio);

    // GROUP BY should be much faster than loading all rows
    ASSERT_TRUE(t1 - t0 < 2000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 7: Descendant PIDs with deep tree
// ==================================================================
void test_scale_descendant_deep() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    int64_t t0 = now_us();
    std::set<int> desc;
    ASSERT_TRUE(db.get_descendant_pids(1, desc));
    int64_t t1 = now_us();

    // Root has all other processes as descendants (50 groups + 50*99 children = 5000)
    ASSERT_TRUE((int)desc.size() >= 5000);

    // Recursive CTE should complete fast
    ASSERT_TRUE(t1 - t0 < 2000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 8: Batch PID queries
// ==================================================================
void test_scale_batch_pids() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    // Build a set of 500 PIDs
    std::set<int> pids;
    for (int i = 1; i <= 500; ++i) pids.insert(i);

    // Batch file access query
    int64_t t0 = now_us();
    std::vector<FileAccessRecord> fa;
    ASSERT_TRUE(db.get_file_accesses_by_pids(pids, fa));
    int64_t t1 = now_us();
    ASSERT_TRUE(!fa.empty());
    ASSERT_TRUE(t1 - t0 < 2000000);

    // Batch process query
    int64_t t2 = now_us();
    std::map<int, ProcessRecord> proc_map;
    ASSERT_TRUE(db.get_processes_by_pids(pids, proc_map));
    int64_t t3 = now_us();
    ASSERT_EQ((int)proc_map.size(), 500);
    ASSERT_TRUE(t3 - t2 < 1000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 9: Hotfile stats (SQL)
// ==================================================================
void test_scale_hotfile_stats() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    int file_count = 0;
    std::string worst_file;
    int worst_procs = 0;

    int64_t t0 = now_us();
    ASSERT_TRUE(db.get_hotfile_stats(10, file_count, worst_file, worst_procs));
    int64_t t1 = now_us();

    // stdio.h is read by 2000 processes — should be the worst
    ASSERT_TRUE(file_count >= 1);
    ASSERT_STR_EQ(worst_file.c_str(), "/usr/include/stdio.h");
    ASSERT_TRUE(worst_procs >= 2000);

    // SQL query should be fast
    ASSERT_TRUE(t1 - t0 < 2000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 10: Scan callback performance
// ==================================================================

struct ScanCountCtx {
    int total;
    int reads;
    int writes;
    ScanCountCtx() : total(0), reads(0), writes(0) {}
};

static void scan_count_cb(int pid, const char* filename, int mode, void* user_data) {
    (void)pid; (void)filename;
    ScanCountCtx* ctx = (ScanCountCtx*)user_data;
    ctx->total++;
    if (mode == FA_READ || mode == FA_RDWR || mode == FA_STAT
        || mode == FA_ACCESS || mode == FA_EXEC || mode == FA_READLINK)
        ctx->reads++;
    if (mode == FA_WRITE || mode == FA_RDWR || mode == FA_RENAME_DST
        || mode == FA_LINK_DST || mode == FA_SYMLINK_LINK
        || mode == FA_TRUNCATE || mode == FA_MKNOD || mode == FA_MKDIR)
        ctx->writes++;
}

void test_scale_scan_callback() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    ScanCountCtx ctx;
    int64_t t0 = now_us();
    int n = db.scan_all_file_accesses(scan_count_cb, &ctx);
    int64_t t1 = now_us();

    ASSERT_TRUE(n >= 50000);
    ASSERT_EQ(n, ctx.total);
    ASSERT_TRUE(ctx.reads > 0);
    ASSERT_TRUE(ctx.writes > 0);

    // Scan should be fast (no object allocation)
    ASSERT_TRUE(t1 - t0 < 2000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 11: Schema upgrade on populated database
// ==================================================================
void test_scale_schema_upgrade() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    // Insert data
    ASSERT_TRUE(db.begin_transaction());
    for (int i = 0; i < 100; ++i) {
        ProcessRecord rec;
        rec.pid = 100 + i;
        rec.ppid = 1;
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "gcc file%d.c", i);
        rec.cmdline = cmd;
        rec.start_time_us = 1000000 + i * 1000;
        rec.end_time_us = rec.start_time_us + 500;
        rec.exit_code = 0;
        ASSERT_TRUE(db.insert_process(rec));
        for (int f = 0; f < 5; ++f) {
            FileAccessRecord fa;
            fa.pid = rec.pid;
            char fname[128];
            std::snprintf(fname, sizeof(fname), "/src/file%d_%d.c", i, f);
            fa.filename = fname;
            fa.mode = (f < 3) ? FA_READ : FA_WRITE;
            fa.fd = 3;
            ASSERT_TRUE(db.insert_file_access(fa));
        }
    }
    ASSERT_TRUE(db.commit_transaction());

    // Simulate re-opening with upgrade
    db.close();
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.upgrade_schema());

    // Schema already at v5, so upgrade_schema() won't auto-populate counts.
    // Call populate_counts() explicitly, then verify.
    ASSERT_TRUE(db.populate_counts());

    ProcessRecord proc;
    ASSERT_TRUE(db.get_process(100, proc));
    ASSERT_EQ(proc.file_count, 5);

    db.close();
    cleanup();
}

// ==================================================================
// Test 12: Root processes query
// ==================================================================
void test_scale_root_processes() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    int64_t t0 = now_us();
    std::vector<ProcessRecord> roots;
    ASSERT_TRUE(db.get_root_processes(roots));
    int64_t t1 = now_us();

    // Should find exactly 1 root (pid=1, ppid=0)
    ASSERT_EQ((int)roots.size(), 1);
    ASSERT_EQ(roots[0].pid, 1);

    // Should be fast
    ASSERT_TRUE(t1 - t0 < 1000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 13: PIDs with children query
// ==================================================================
void test_scale_pids_with_children() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));

    int64_t t0 = now_us();
    std::set<int> has_children;
    ASSERT_TRUE(db.get_pids_with_children(has_children));
    int64_t t1 = now_us();

    // Root (pid=1) and group parents should have children
    ASSERT_TRUE(has_children.find(1) != has_children.end());
    ASSERT_TRUE((int)has_children.size() >= 51);  // root + 50 group parents

    ASSERT_TRUE(t1 - t0 < 1000000);

    db.close();
    cleanup();
}

// ==================================================================
// Test 14: Concurrent reads (WAL mode)
// ==================================================================
void test_scale_concurrent_reads() {
    cleanup();
    Database db1;
    ASSERT_TRUE(db1.open(TEST_DB));
    ASSERT_TRUE(db1.init_schema());

    // Insert some data
    ASSERT_TRUE(db1.begin_transaction());
    for (int i = 0; i < 100; ++i) {
        ProcessRecord rec;
        rec.pid = 100 + i;
        rec.ppid = 1;
        rec.cmdline = "test";
        rec.start_time_us = 1000000 + i;
        ASSERT_TRUE(db1.insert_process(rec));
    }
    ASSERT_TRUE(db1.commit_transaction());

    // Open second connection for reading
    Database db2;
    ASSERT_TRUE(db2.open(TEST_DB));
    ASSERT_TRUE(db2.upgrade_schema());

    // Both should be able to read
    std::vector<ProcessRecord> procs1, procs2;
    ASSERT_TRUE(db1.get_all_processes(procs1));
    ASSERT_TRUE(db2.get_all_processes(procs2));
    ASSERT_EQ((int)procs1.size(), (int)procs2.size());

    db2.close();
    db1.close();
    cleanup();
}

// ==================================================================
// Test 15: Empty database handling
// ==================================================================
void test_scale_empty_db() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    // All queries should handle empty DB gracefully
    std::vector<ProcessRecord> procs;
    ASSERT_TRUE(db.get_all_processes(procs));
    ASSERT_EQ((int)procs.size(), 0);

    std::vector<ProcessRecord> roots;
    ASSERT_TRUE(db.get_root_processes(roots));
    ASSERT_EQ((int)roots.size(), 0);

    std::set<int> has_children;
    ASSERT_TRUE(db.get_pids_with_children(has_children));
    ASSERT_EQ((int)has_children.size(), 0);

    std::vector<FileStatRow> stats;
    ASSERT_TRUE(db.get_file_stats_grouped(stats));
    ASSERT_EQ((int)stats.size(), 0);

    int hf_count = 0;
    std::string worst;
    int worst_n = 0;
    ASSERT_TRUE(db.get_hotfile_stats(10, hf_count, worst, worst_n));
    ASSERT_EQ(hf_count, 0);

    std::set<int> empty_pids;
    std::vector<FileAccessRecord> fa;
    ASSERT_TRUE(db.get_file_accesses_by_pids(empty_pids, fa));
    ASSERT_EQ((int)fa.size(), 0);

    std::map<int, ProcessRecord> pm;
    ASSERT_TRUE(db.get_processes_by_pids(empty_pids, pm));
    ASSERT_EQ((int)pm.size(), 0);

    ASSERT_EQ(db.get_process_count(), 0);
    ASSERT_EQ(db.get_file_access_count(), 0);
    ASSERT_EQ(db.get_failed_access_count(), 0);

    db.close();
    cleanup();
}

// ==================================================================
// Test 16: Special characters in filenames and cmdlines
// ==================================================================
void test_scale_special_chars() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    // Insert process with special cmdline
    ProcessRecord rec;
    rec.pid = 100;
    rec.ppid = 1;
    rec.cmdline = "echo 'hello \"world\"' > /tmp/file with spaces.txt";
    rec.cwd = "/path with spaces/build";
    rec.start_time_us = 1000000;
    rec.end_time_us = 2000000;
    rec.exit_code = 0;
    ASSERT_TRUE(db.insert_process(rec));

    // Insert file access with special characters in filename
    FileAccessRecord fa;
    fa.pid = 100;
    fa.filename = "/tmp/file with spaces.txt";
    fa.mode = FA_WRITE;
    fa.fd = 1;
    ASSERT_TRUE(db.insert_file_access(fa));

    FileAccessRecord fa2;
    fa2.pid = 100;
    fa2.filename = "/build/src/file'quote.c";
    fa2.mode = FA_READ;
    fa2.fd = 3;
    ASSERT_TRUE(db.insert_file_access(fa2));

    // Retrieve and verify
    ProcessRecord out;
    ASSERT_TRUE(db.get_process(100, out));
    ASSERT_STR_EQ(out.cmdline.c_str(), "echo 'hello \"world\"' > /tmp/file with spaces.txt");
    ASSERT_STR_EQ(out.cwd.c_str(), "/path with spaces/build");

    std::vector<FileAccessRecord> accesses;
    ASSERT_TRUE(db.get_file_accesses_by_pid(100, accesses));
    ASSERT_EQ((int)accesses.size(), 2);

    // Query by filename with special chars
    std::vector<FileAccessRecord> by_name;
    ASSERT_TRUE(db.get_file_accesses_by_name("/tmp/file with spaces.txt", by_name));
    ASSERT_EQ((int)by_name.size(), 1);

    std::vector<FileAccessRecord> by_name2;
    ASSERT_TRUE(db.get_file_accesses_by_name("/build/src/file'quote.c", by_name2));
    ASSERT_EQ((int)by_name2.size(), 1);

    db.close();
    cleanup();
}

// ==================================================================
// Test 17: Very long cmdline
// ==================================================================
void test_scale_long_cmdline() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    // Build a very long cmdline (10KB)
    std::string long_cmd = "gcc";
    for (int i = 0; i < 500; ++i) {
        char arg[32];
        std::snprintf(arg, sizeof(arg), " -I/very/long/path/%d", i);
        long_cmd += arg;
    }

    ProcessRecord rec;
    rec.pid = 200;
    rec.ppid = 1;
    rec.cmdline = long_cmd;
    rec.start_time_us = 1000000;
    rec.end_time_us = 2000000;
    rec.exit_code = 0;
    ASSERT_TRUE(db.insert_process(rec));

    ProcessRecord out;
    ASSERT_TRUE(db.get_process(200, out));
    ASSERT_STR_EQ(out.cmdline.c_str(), long_cmd.c_str());

    db.close();
    cleanup();
}

// ==================================================================
// Test 18: Very long filepath
// ==================================================================
void test_scale_long_filepath() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    ProcessRecord rec;
    rec.pid = 300;
    rec.ppid = 1;
    rec.cmdline = "cat";
    rec.start_time_us = 1000000;
    ASSERT_TRUE(db.insert_process(rec));

    // Build a very long filepath (4KB)
    std::string long_path = "/";
    for (int i = 0; i < 200; ++i) {
        char seg[32];
        std::snprintf(seg, sizeof(seg), "subdir%d/", i);
        long_path += seg;
    }
    long_path += "file.txt";

    FileAccessRecord fa;
    fa.pid = 300;
    fa.filename = long_path;
    fa.mode = FA_READ;
    fa.fd = 3;
    ASSERT_TRUE(db.insert_file_access(fa));

    // Retrieve by pid
    std::vector<FileAccessRecord> accesses;
    ASSERT_TRUE(db.get_file_accesses_by_pid(300, accesses));
    ASSERT_EQ((int)accesses.size(), 1);
    ASSERT_STR_EQ(accesses[0].filename.c_str(), long_path.c_str());

    // Retrieve by exact filename
    std::vector<FileAccessRecord> by_name;
    ASSERT_TRUE(db.get_file_accesses_by_name(long_path, by_name));
    ASSERT_EQ((int)by_name.size(), 1);

    db.close();
    cleanup();
}

// ==================================================================
// Test 19: Data integrity — file_count/fail_count consistency
// ==================================================================
void test_scale_count_consistency() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    ASSERT_TRUE(populate_large_db(db));
    ASSERT_TRUE(db.populate_counts());

    // For each process, verify file_count matches actual count
    std::vector<ProcessRecord> procs;
    ASSERT_TRUE(db.get_all_processes(procs));

    // Check a sample of processes
    for (int sample = 0; sample < 20; ++sample) {
        int idx = sample * ((int)procs.size() / 20);
        if (idx >= (int)procs.size()) break;

        std::vector<FileAccessRecord> fa;
        ASSERT_TRUE(db.get_file_accesses_by_pid(procs[idx].pid, fa));
        ASSERT_EQ(procs[idx].file_count, (int)fa.size());
    }

    db.close();
    cleanup();
}

// ==================================================================
// Test 20: get_file_stats_grouped matches manual aggregation
// ==================================================================
void test_scale_file_stats_accuracy() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    // Insert known data
    ProcessRecord p1; p1.pid = 10; p1.ppid = 1; p1.cmdline = "a"; p1.start_time_us = 1;
    ProcessRecord p2; p2.pid = 20; p2.ppid = 1; p2.cmdline = "b"; p2.start_time_us = 2;
    ASSERT_TRUE(db.insert_process(p1));
    ASSERT_TRUE(db.insert_process(p2));

    // file1: pid=10 reads, pid=20 reads -> read_count=2, process_count=2
    FileAccessRecord fa;
    fa.fd = 3;
    fa.pid = 10; fa.filename = "/file1"; fa.mode = FA_READ; db.insert_file_access(fa);
    fa.pid = 20; fa.filename = "/file1"; fa.mode = FA_READ; db.insert_file_access(fa);
    // file2: pid=10 writes -> write_count=1, process_count=1
    fa.pid = 10; fa.filename = "/file2"; fa.mode = FA_WRITE; db.insert_file_access(fa);
    // file3: pid=10 rdwr -> read_count=1, write_count=1
    fa.pid = 10; fa.filename = "/file3"; fa.mode = FA_RDWR; db.insert_file_access(fa);
    // file4: pid=10 stat + pid=20 exec -> read_count=2
    fa.pid = 10; fa.filename = "/file4"; fa.mode = FA_STAT; db.insert_file_access(fa);
    fa.pid = 20; fa.filename = "/file4"; fa.mode = FA_EXEC; db.insert_file_access(fa);

    std::vector<FileStatRow> stats;
    ASSERT_TRUE(db.get_file_stats_grouped(stats));
    ASSERT_EQ((int)stats.size(), 4);

    // Build map for easy lookup
    std::map<std::string, FileStatRow> m;
    for (size_t i = 0; i < stats.size(); ++i) m[stats[i].filename] = stats[i];

    ASSERT_EQ(m["/file1"].access_count, 2);
    ASSERT_EQ(m["/file1"].read_count, 2);
    ASSERT_EQ(m["/file1"].write_count, 0);
    ASSERT_EQ(m["/file1"].process_count, 2);

    ASSERT_EQ(m["/file2"].access_count, 1);
    ASSERT_EQ(m["/file2"].read_count, 0);
    ASSERT_EQ(m["/file2"].write_count, 1);
    ASSERT_EQ(m["/file2"].process_count, 1);

    ASSERT_EQ(m["/file3"].access_count, 1);
    ASSERT_EQ(m["/file3"].read_count, 1);
    ASSERT_EQ(m["/file3"].write_count, 1);
    ASSERT_EQ(m["/file3"].process_count, 1);

    ASSERT_EQ(m["/file4"].access_count, 2);
    ASSERT_EQ(m["/file4"].read_count, 2);
    ASSERT_EQ(m["/file4"].write_count, 0);
    ASSERT_EQ(m["/file4"].process_count, 2);

    db.close();
    cleanup();
}

void run_scalability_tests() {
    std::printf("=== Scalability Tests ===\n");

    RUN_TEST(test_scale_insert);
    RUN_TEST(test_scale_query_all_processes);
    RUN_TEST(test_scale_query_fa_by_pid);
    RUN_TEST(test_scale_query_fa_by_prefix);
    RUN_TEST(test_scale_populate_counts);
    RUN_TEST(test_scale_file_stats_grouped);
    RUN_TEST(test_scale_descendant_deep);
    RUN_TEST(test_scale_batch_pids);
    RUN_TEST(test_scale_hotfile_stats);
    RUN_TEST(test_scale_scan_callback);

    std::printf("=== Schema & Upgrade Tests ===\n");

    RUN_TEST(test_scale_schema_upgrade);

    std::printf("=== Edge Case & Robustness Tests ===\n");

    RUN_TEST(test_scale_root_processes);
    RUN_TEST(test_scale_pids_with_children);
    RUN_TEST(test_scale_concurrent_reads);
    RUN_TEST(test_scale_empty_db);
    RUN_TEST(test_scale_special_chars);
    RUN_TEST(test_scale_long_cmdline);
    RUN_TEST(test_scale_long_filepath);
    RUN_TEST(test_scale_count_consistency);
    RUN_TEST(test_scale_file_stats_accuracy);
}
