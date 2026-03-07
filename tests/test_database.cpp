#include "helpers/test_helpers.h"
#include "../src/common/types.h"
#include "../src/common/log.h"
#include "../src/db/database.h"

#include <unistd.h>
#include <string>
#include <vector>

using namespace bdtrace;

static const char* TEST_DB = "/tmp/bdtrace_test.db";

static void cleanup() {
    unlink(TEST_DB);
}

void test_open_close() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());
    db.close();
    cleanup();
}

void test_insert_process() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    ProcessRecord rec;
    rec.pid = 100;
    rec.ppid = 1;
    rec.cmdline = "/bin/true";
    rec.start_time_us = 1000000;
    rec.end_time_us = 2000000;
    rec.exit_code = 0;

    ASSERT_TRUE(db.insert_process(rec));

    ProcessRecord out;
    ASSERT_TRUE(db.get_process(100, out));
    ASSERT_EQ(out.pid, 100);
    ASSERT_EQ(out.ppid, 1);
    ASSERT_STR_EQ(out.cmdline.c_str(), "/bin/true");
    ASSERT_EQ(out.start_time_us, 1000000);
    ASSERT_EQ(out.end_time_us, 2000000);
    ASSERT_EQ(out.exit_code, 0);

    db.close();
    cleanup();
}

void test_update_exit() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    ProcessRecord rec;
    rec.pid = 200;
    rec.ppid = 1;
    rec.cmdline = "sleep 1";
    rec.start_time_us = 1000000;
    ASSERT_TRUE(db.insert_process(rec));
    ASSERT_TRUE(db.update_process_exit(200, 3000000, 0));

    ProcessRecord out;
    ASSERT_TRUE(db.get_process(200, out));
    ASSERT_EQ(out.end_time_us, 3000000);
    ASSERT_EQ(out.exit_code, 0);

    db.close();
    cleanup();
}

void test_file_access() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    ProcessRecord proc;
    proc.pid = 300;
    proc.ppid = 1;
    proc.cmdline = "cat /etc/hosts";
    proc.start_time_us = 1000000;
    ASSERT_TRUE(db.insert_process(proc));

    FileAccessRecord fa;
    fa.pid = 300;
    fa.filename = "/etc/hosts";
    fa.mode = FA_READ;
    fa.fd = 3;
    ASSERT_TRUE(db.insert_file_access(fa));

    std::vector<FileAccessRecord> accesses;
    ASSERT_TRUE(db.get_file_accesses_by_pid(300, accesses));
    ASSERT_EQ((int)accesses.size(), 1);
    ASSERT_STR_EQ(accesses[0].filename.c_str(), "/etc/hosts");
    ASSERT_EQ(accesses[0].mode, FA_READ);

    std::vector<FileAccessRecord> by_name;
    ASSERT_TRUE(db.get_file_accesses_by_name("/etc/hosts", by_name));
    ASSERT_EQ((int)by_name.size(), 1);

    db.close();
    cleanup();
}

void test_children() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    ProcessRecord parent;
    parent.pid = 400;
    parent.ppid = 1;
    parent.cmdline = "make";
    parent.start_time_us = 1000000;
    ASSERT_TRUE(db.insert_process(parent));

    ProcessRecord child1;
    child1.pid = 401;
    child1.ppid = 400;
    child1.cmdline = "gcc -c a.c";
    child1.start_time_us = 1100000;
    ASSERT_TRUE(db.insert_process(child1));

    ProcessRecord child2;
    child2.pid = 402;
    child2.ppid = 400;
    child2.cmdline = "gcc -c b.c";
    child2.start_time_us = 1200000;
    ASSERT_TRUE(db.insert_process(child2));

    std::vector<ProcessRecord> children;
    ASSERT_TRUE(db.get_children(400, children));
    ASSERT_EQ((int)children.size(), 2);

    db.close();
    cleanup();
}

void test_transaction() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    ASSERT_TRUE(db.begin_transaction());
    for (int i = 0; i < 100; ++i) {
        ProcessRecord rec;
        rec.pid = 500 + i;
        rec.ppid = 1;
        rec.cmdline = "worker";
        rec.start_time_us = 1000000 + i * 1000;
        ASSERT_TRUE(db.insert_process(rec));
    }
    ASSERT_TRUE(db.commit_transaction());

    ASSERT_EQ(db.get_process_count(), 100);

    db.close();
    cleanup();
}

void test_meta() {
    cleanup();
    Database db;
    ASSERT_TRUE(db.open(TEST_DB));
    ASSERT_TRUE(db.init_schema());

    ASSERT_TRUE(db.insert_meta("version", "1.0"));
    ASSERT_TRUE(db.insert_meta("command", "make -j4"));

    db.close();
    cleanup();
}

void run_database_tests() {
    std::printf("=== Database Tests ===\n");

    RUN_TEST(test_open_close);
    RUN_TEST(test_insert_process);
    RUN_TEST(test_update_exit);
    RUN_TEST(test_file_access);
    RUN_TEST(test_children);
    RUN_TEST(test_transaction);
    RUN_TEST(test_meta);
}
