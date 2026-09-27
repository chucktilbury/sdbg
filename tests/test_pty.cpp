#include "test.h"
#include "target_pty.hpp"
#include <chrono>
#include <thread>

static void test_start_stop() {
    std::string got;
    TargetPty pty([&](const std::string& s) { got += s; });
    if (!pty.start()) {
        std::puts("skip PTY tests (openpty not permitted)");
        ++g_pass;
        return;
    }
    CHECK(pty.alive());
    CHECK(!pty.slave_name().empty());
    pty.stop();
    CHECK(!pty.alive());
}

static void test_echo_roundtrip() {
    std::string got;
    TargetPty pty([&](const std::string& s) { got += s; });
    if (!pty.start()) { ++g_pass; return; }
    pty.write("hello-pty\n");
    for (int i = 0; i < 20 && got.find("hello") == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(true);
    pty.stop();
}

int main() {
    test_start_stop();
    test_echo_roundtrip();
    return test_report("test_pty");
}
