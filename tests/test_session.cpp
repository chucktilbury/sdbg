#include "test.h"
#include "session.hpp"

#include <fstream>

static bool have_bin(const std::string& name) {
    return !which_bin({name}).empty() && access(which_bin({name}).c_str(), X_OK) == 0;
}

static void test_session_start_gdb() {
    if (!have_bin("gdb")) {
        std::puts("skip test_session_start_gdb (no gdb)");
        return;
    }
    std::string out;
    auto b = make_gdb({});
    // Use CLI, not MI, so "help" is predictable
    b.argv = {b.binary, "-q", "--nh", "-ex", "set pagination off"};
    DebuggerSession s(b, [&](const std::string& t) { out += t; });
    CHECK(s.start());
    CHECK(s.alive());
    try {
        auto r = s.command("help", 4.0);
        CHECK(!r.empty() || !out.empty());
    } catch (...) {
        CHECK(false);
    }
    s.command_async(" ");
    s.stop();
    CHECK(!s.alive());
}

static void test_session_dead_command() {
    auto b = make_gdb("/usr/bin/gdb");
    DebuggerSession s(b, [](const std::string&) {});
    bool threw = false;
    try {
        s.command("bt");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    test_session_dead_command();
    test_session_start_gdb();
    return test_report("test_session");
}
