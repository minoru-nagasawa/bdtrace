#include "helpers/test_helpers.h"
#include "../src/common/view_helpers.h"

#include <string>

using namespace bdtrace;

static void test_simple_command() {
    ASSERT_STR_EQ(cmd_name("gcc").c_str(), "gcc");
    ASSERT_STR_EQ(cmd_name("/usr/bin/gcc").c_str(), "gcc");
    ASSERT_STR_EQ(cmd_name("gcc -c foo.c -o foo.o").c_str(), "gcc");
    ASSERT_STR_EQ(cmd_name("/usr/bin/gcc -c foo.c").c_str(), "gcc");
}

static void test_perl_script() {
    ASSERT_STR_EQ(cmd_name("perl configure.pl").c_str(), "configure.pl");
    ASSERT_STR_EQ(cmd_name("/usr/bin/perl mkbuildinf.pl").c_str(), "mkbuildinf.pl");
    ASSERT_STR_EQ(cmd_name("perl -w configure.pl --prefix=/usr").c_str(), "configure.pl");
    ASSERT_STR_EQ(cmd_name("perl -I lib script.pl").c_str(), "script.pl");
}

static void test_python_script() {
    ASSERT_STR_EQ(cmd_name("python build.py").c_str(), "build.py");
    ASSERT_STR_EQ(cmd_name("python3 /path/to/setup.py install").c_str(), "setup.py");
    ASSERT_STR_EQ(cmd_name("python2 -u script.py").c_str(), "script.py");
}

static void test_shell_script() {
    ASSERT_STR_EQ(cmd_name("sh configure").c_str(), "configure");
    ASSERT_STR_EQ(cmd_name("bash /path/to/build.sh").c_str(), "build.sh");
    ASSERT_STR_EQ(cmd_name("/bin/sh -e install.sh").c_str(), "install.sh");
    ASSERT_STR_EQ(cmd_name("dash -x run.sh arg1").c_str(), "run.sh");
}

static void test_env_wrapper() {
    ASSERT_STR_EQ(cmd_name("env perl script.pl").c_str(), "script.pl");
    ASSERT_STR_EQ(cmd_name("env PATH=/usr/bin CC=gcc perl build.pl").c_str(), "build.pl");
    ASSERT_STR_EQ(cmd_name("/usr/bin/env python3 setup.py").c_str(), "setup.py");
    ASSERT_STR_EQ(cmd_name("env -i perl test.pl").c_str(), "test.pl");
    // env wrapping a non-interpreter
    ASSERT_STR_EQ(cmd_name("env CC=gcc make -j4").c_str(), "make");
}

static void test_interpreter_no_script() {
    // Bare interpreter with no arguments: return interpreter name
    ASSERT_STR_EQ(cmd_name("perl").c_str(), "perl");
    ASSERT_STR_EQ(cmd_name("python3").c_str(), "python3");
    // Interpreter with only flags: return interpreter name
    ASSERT_STR_EQ(cmd_name("perl -e print 1").c_str(), "perl");
    ASSERT_STR_EQ(cmd_name("bash -c echo hello").c_str(), "bash");
}

static void test_node_ruby() {
    ASSERT_STR_EQ(cmd_name("node server.js").c_str(), "server.js");
    ASSERT_STR_EQ(cmd_name("ruby /path/to/gen.rb").c_str(), "gen.rb");
}

static void test_awk_sed() {
    // awk/sed: first non-flag arg is code, not script
    ASSERT_STR_EQ(cmd_name("awk {print $1} file.txt").c_str(), "awk");
    ASSERT_STR_EQ(cmd_name("gawk -F: {print $1} /etc/passwd").c_str(), "gawk");
    ASSERT_STR_EQ(cmd_name("sed s/foo/bar/g input.txt").c_str(), "sed");
    // -f specifies a script file
    ASSERT_STR_EQ(cmd_name("awk -f process.awk data.txt").c_str(), "process.awk");
    ASSERT_STR_EQ(cmd_name("sed -f /path/to/transform.sed input").c_str(), "transform.sed");
    ASSERT_STR_EQ(cmd_name("gawk -F: -f script.awk").c_str(), "script.awk");
}

static void test_m4_java_cmake() {
    ASSERT_STR_EQ(cmd_name("m4 macros.m4").c_str(), "macros.m4");
    ASSERT_STR_EQ(cmd_name("java -jar build.jar").c_str(), "build.jar");
    ASSERT_STR_EQ(cmd_name("cmake -P script.cmake").c_str(), "script.cmake");
    ASSERT_STR_EQ(cmd_name("php generate.php").c_str(), "generate.php");
}

// --- cmd_name_full tests (absolute path resolution) ---

static void test_full_relative_script() {
    // Relative paths resolved against cwd
    ASSERT_STR_EQ(cmd_name_full("perl util/mkbuildinf.pl", "/src/openssl").c_str(),
                  "/src/openssl/util/mkbuildinf.pl");
    ASSERT_STR_EQ(cmd_name_full("perl ./util/mkbuildinf.pl", "/src/openssl").c_str(),
                  "/src/openssl/./util/mkbuildinf.pl");
}

static void test_full_absolute_script() {
    // Absolute paths stay as-is
    ASSERT_STR_EQ(cmd_name_full("perl /usr/share/gen.pl", "/home/user").c_str(),
                  "/usr/share/gen.pl");
}

static void test_full_non_interpreter() {
    // Non-interpreters return basename (same as cmd_name)
    ASSERT_STR_EQ(cmd_name_full("gcc -c foo.c", "/home/user").c_str(), "gcc");
    ASSERT_STR_EQ(cmd_name_full("/usr/bin/gcc -c foo.c", "/home/user").c_str(), "gcc");
}

static void test_full_same_script_different_paths() {
    // Same basename but different directories -> different full paths
    std::string a = cmd_name_full("perl tools/gen.pl", "/project");
    std::string b = cmd_name_full("perl lib/gen.pl", "/project");
    ASSERT_TRUE(a != b);
    ASSERT_STR_EQ(a.c_str(), "/project/tools/gen.pl");
    ASSERT_STR_EQ(b.c_str(), "/project/lib/gen.pl");
}

static void test_full_empty_cwd() {
    // Empty cwd: return path as-is
    ASSERT_STR_EQ(cmd_name_full("perl util/gen.pl", "").c_str(), "util/gen.pl");
}

static void test_full_env_wrapper() {
    ASSERT_STR_EQ(cmd_name_full("env perl util/gen.pl", "/build").c_str(),
                  "/build/util/gen.pl");
    ASSERT_STR_EQ(cmd_name_full("env CC=gcc make -j4", "/build").c_str(), "make");
}

void run_cmd_name_tests() {
    std::printf("cmd_name tests:\n");
    RUN_TEST(test_simple_command);
    RUN_TEST(test_perl_script);
    RUN_TEST(test_python_script);
    RUN_TEST(test_shell_script);
    RUN_TEST(test_env_wrapper);
    RUN_TEST(test_interpreter_no_script);
    RUN_TEST(test_node_ruby);
    RUN_TEST(test_awk_sed);
    RUN_TEST(test_m4_java_cmake);
    RUN_TEST(test_full_relative_script);
    RUN_TEST(test_full_absolute_script);
    RUN_TEST(test_full_non_interpreter);
    RUN_TEST(test_full_same_script_different_paths);
    RUN_TEST(test_full_empty_cwd);
    RUN_TEST(test_full_env_wrapper);
}
