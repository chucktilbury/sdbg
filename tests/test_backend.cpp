#include "test.h"
#include "backend.hpp"

#include <algorithm>
#include <unistd.h>

static bool contains(const std::vector<std::string>& v, const std::string& sub) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) {
        return s.find(sub) != std::string::npos;
    });
}

static void test_json() {
    CHECK_STREQ(json_escape("a\"b\\c\nd"), "a\\\"b\\\\c\\nd");
    auto src = std::string("{\"debugger\":\"gdb\",\"stop_at_entry\":true,\"n\":12}");
    CHECK_STREQ(json_get(src, "debugger"), "gdb");
    CHECK_STREQ(json_get(src, "stop_at_entry"), "true");
    CHECK_STREQ(json_get(src, "missing"), "");
}

static void test_env() {
    auto e = Backend::parse_env("A=1, B=2\nC=3");
    CHECK_EQ(e.size(), 3u);
    CHECK_STREQ(e[0], "A=1");
    CHECK_STREQ(e[1], "B=2");
    CHECK_STREQ(e[2], "C=3");
    CHECK(Backend::parse_env("nope").empty());
    CHECK(Backend::parse_env("").empty());
}

static void test_gdb_commands() {
    auto g = make_gdb("/usr/bin/gdb");
    CHECK_STREQ(g.name, "gdb");
    CHECK_STREQ(g.continue_cmd, "continue");
    CHECK_STREQ(g.run_cmd(false), "run");
    CHECK_STREQ(g.run_cmd(true), "start");
    CHECK_STREQ(g.select_frame(3), "frame 3");
    CHECK_STREQ(g.select_thread(2), "thread 2");
    CHECK_STREQ(g.eval_cmd("x+1"), "print x+1");
    CHECK_STREQ(g.attach_cmd(42), "attach 42");
    CHECK_STREQ(g.delete_bp_id("3"), "delete breakpoints 3");
    CHECK_STREQ(g.enable_bp_id("3", false), "disable 3");
    CHECK_STREQ(g.enable_bp_id("3", true), "enable 3");
    CHECK_STREQ(g.mem_cmd("0x1000", 32), "x/32xb 0x1000");
    CHECK_STREQ(g.write_reg_cmd("rax", "1"), "set $rax = 1");
    CHECK_STREQ(g.radix_cmd(true), "set output-radix 16");
    CHECK_STREQ(g.radix_cmd(false), "set output-radix 10");
    CHECK_STREQ(g.expand_local_cmd("node"), "p node");
    CHECK_STREQ(g.breakpoint("main"), "break main");
    CHECK_STREQ(g.breakpoint("a.c:10"), "break a.c:10");
    CHECK_STREQ(g.breakpoint("foo", "i==3", "1"), "break foo thread 1 if i==3");
    CHECK_STREQ(g.remote_cmd("host:9999"), "target remote host:9999");
    CHECK_STREQ(g.core_cmd("/tmp/core", "./a.out"), "core-file \"/tmp/core\"");
    CHECK_STREQ(g.source_map_cmd("/b", "/s"), "set substitute-path /b /s");
    auto load = g.load_target("/bin/ls");
    CHECK(contains(load, "file \"/bin/ls\""));
    auto p = g.apply_params("-v", "/tmp", "A=1", "#cmt\nset print pretty on", true);
    CHECK(contains(p, "cd \"/tmp\""));
    CHECK(contains(p, "set args -v"));
    CHECK(contains(p, "set environment A=1"));
    CHECK(contains(p, "set print pretty on"));
    CHECK(!contains(p, "#cmt"));
    auto io = g.io_cmds("/in", "/out", "/err", "/dev/pts/3", "a b");
    CHECK(contains(io, "tty /dev/pts/3"));
    CHECK(contains(io, "< \"/in\""));
    CHECK(contains(io, "> \"/out\""));
    CHECK(contains(io, "2> \"/err\""));
    auto fk = g.fork_cmds("child", false);
    CHECK(contains(fk, "follow-fork-mode child"));
    CHECK(contains(fk, "detach-on-fork off"));
    auto rl = g.reload_cmds("/bin/ls");
    CHECK(contains(rl, "symbol-file"));
}

static void test_lldb_commands() {
    auto l = make_lldb("/usr/bin/lldb");
    CHECK_STREQ(l.name, "lldb");
    CHECK_STREQ(l.continue_cmd, "process continue");
    CHECK_STREQ(l.run_cmd(true), "run");
    CHECK_STREQ(l.select_frame(1), "frame select 1");
    CHECK_STREQ(l.select_thread(4), "thread select 4");
    CHECK_STREQ(l.eval_cmd("x"), "expression -- x");
    CHECK_STREQ(l.attach_cmd(9), "process attach --pid 9");
    CHECK_STREQ(l.delete_bp_id("2"), "breakpoint delete 2");
    CHECK_STREQ(l.enable_bp_id("2", false), "breakpoint disable 2");
    CHECK_STREQ(l.mem_cmd("buf", 8), "memory read --size 1 --count 8 buf");
    CHECK_STREQ(l.write_reg_cmd("x0", "0"), "register write x0 0");
    CHECK(l.radix_cmd(true).find("hex") != std::string::npos);
    CHECK_STREQ(l.breakpoint("main"), "breakpoint set --name main");
    CHECK(l.breakpoint("f.c:12").find("--file \"f.c\"") != std::string::npos);
    CHECK(l.breakpoint("f.c:12").find("--line 12") != std::string::npos);
    auto cond = l.breakpoint("foo", "i==3", "2");
    CHECK(cond.find("--thread-index 2") != std::string::npos);
    CHECK(cond.find("--condition \"i==3\"") != std::string::npos);
    CHECK_STREQ(l.remote_cmd("1.2.3.4:1234"), "gdb-remote 1.2.3.4:1234");
    auto core = l.core_cmd("/c", "/e");
    CHECK(core.find("--core \"/c\"") != std::string::npos);
    CHECK_STREQ(l.source_map_cmd("/a", "/b"), "settings append target.source-map /a /b");
    auto load = l.load_target("/bin/ls");
    CHECK(contains(load, "platform select host"));
    auto p = l.apply_params("x", "/w", "K=V", "", false);
    CHECK(contains(p, "target.cwd"));
    CHECK(contains(p, "target.run-args x"));
    CHECK(contains(p, "stop-at-entry false"));
    CHECK(contains(p, "target.env-vars K=\"V\""));
    auto io = l.io_cmds("/i", "/o", "/e", "", "");
    CHECK(contains(io, "input-path \"/i\""));
    auto fk = l.fork_cmds("ask", true);
    CHECK(contains(fk, "follow-fork-mode parent"));
    CHECK(contains(fk, "detach-on-fork true"));
}

static void test_config_roundtrip() {
    Config c;
    c.debugger = "gdb";
    c.target = "/bin/ls";
    c.args = "-l";
    c.cwd = "/tmp";
    c.env = "A=1";
    c.stop_at_entry = true;
    c.stdin_path = "/in";
    c.remote = "h:1";
    c.follow_fork = "child";
    c.detach_on_fork = false;
    c.target_console = false;
    c.font_size = 14;
    c.theme = "light";
    c.hex_locals = true;
    c.source_map = "/a=/b";
    c.win_w = 800;
    c.win_h = 600;
    const char* path = "/tmp/debugger-gui-test.json";
    save_config_file(c, path);
    Config d;
    load_config_file(d, path);
    CHECK_STREQ(d.debugger, "gdb");
    CHECK_STREQ(d.target, "/bin/ls");
    CHECK_STREQ(d.args, "-l");
    CHECK_STREQ(d.cwd, "/tmp");
    CHECK_STREQ(d.env, "A=1");
    CHECK(d.stop_at_entry);
    CHECK_STREQ(d.stdin_path, "/in");
    CHECK_STREQ(d.remote, "h:1");
    CHECK_STREQ(d.follow_fork, "child");
    CHECK(!d.detach_on_fork);
    CHECK(!d.target_console);
    CHECK_EQ(d.font_size, 14);
    CHECK_STREQ(d.theme, "light");
    CHECK(d.hex_locals);
    CHECK_STREQ(d.source_map, "/a=/b");
    CHECK_EQ(d.win_w, 800);
    CHECK_EQ(d.win_h, 600);
    unlink(path);
}

static void test_cli() {
    const char* argv[] = {"dbg", "--gdb", "--cwd", "/tmp", "--args", "-v", "--stop-at-entry",
                          "--remote", "localhost:9999", "--follow-fork", "child",
                          "--source-map", "/x=/y", "--break", "main", "--load",
                          "/bin/echo", "--", "hello"};
    int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0]));
    auto cfg = parse_cli(argc, const_cast<char**>(argv));
    CHECK_STREQ(cfg.debugger, "gdb");
    CHECK_STREQ(cfg.cwd, "/tmp");
    CHECK_STREQ(cfg.args, "-v");
    CHECK(cfg.stop_at_entry);
    CHECK_STREQ(cfg.remote, "localhost:9999");
    CHECK_STREQ(cfg.follow_fork, "child");
    CHECK(cfg.source_map.find("/x=/y") != std::string::npos);
    CHECK_EQ(cfg.breakpoints.size(), 1u);
    CHECK_STREQ(cfg.breakpoints[0], "main");
    CHECK(cfg.load);
    CHECK_STREQ(cfg.target, "/bin/echo");
}

static void test_cli_lldb_flags() {
    const char* argv[] = {"dbg", "--lldb", "--no-target-console", "--no-target-echo",
                          "--stdin", "/dev/null", "--attach", "1234"};
    int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0]));
    auto cfg = parse_cli(argc, const_cast<char**>(argv));
    CHECK_STREQ(cfg.debugger, "lldb");
    CHECK(!cfg.target_console);
    CHECK(!cfg.target_echo);
    CHECK_STREQ(cfg.stdin_path, "/dev/null");
    CHECK_EQ(cfg.attach, 1234);
}

static void test_which() {
    auto sh = which_bin({"sh", "does-not-exist-xyz"});
    CHECK(!sh.empty());
    CHECK(sh.find("sh") != std::string::npos);
}

int main() {
    test_json();
    test_env();
    test_gdb_commands();
    test_lldb_commands();
    test_config_roundtrip();
    test_cli();
    test_cli_lldb_flags();
    test_which();
    return test_report("test_backend");
}
