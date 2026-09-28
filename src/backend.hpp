#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

struct Backend {
    std::string name;
    std::string binary;
    std::string prompt;
    std::vector<std::string> argv;
    std::string continue_cmd;
    std::string step_in;
    std::string step_over;
    std::string step_out;
    std::string kill_cmd;
    std::string threads_cmd;
    std::string locals_cmd;
    std::string regs_cmd;
    std::string breaks_cmd;
    std::string frame_info_cmd;
    std::string frame_select; // contains {n}
    std::string thread_select; // contains {n}
    std::string delete_breaks;
    std::string disasm_cmd;

    std::string select_thread(int n) const {
        std::string s = thread_select;
        auto p = s.find("{n}");
        if (p != std::string::npos) s.replace(p, 3, std::to_string(n));
        return s;
    }

    std::string eval_cmd(const std::string& expr) const {
        if (name == "gdb") return "print " + expr;
        return "expression -- " + expr;
    }

    std::string attach_cmd(int pid) const {
        if (name == "gdb") return "attach " + std::to_string(pid);
        return "process attach --pid " + std::to_string(pid);
    }

    std::string delete_bp_id(const std::string& id) const {
        if (name == "gdb") return "delete breakpoints " + id;
        return "breakpoint delete " + id;
    }

    std::string enable_bp_id(const std::string& id, bool on) const {
        if (name == "gdb") return std::string(on ? "enable " : "disable ") + id;
        return std::string(on ? "breakpoint enable " : "breakpoint disable ") + id;
    }

    std::string mem_cmd(const std::string& addr, int count = 64) const {
        if (name == "gdb") return "x/" + std::to_string(count) + "xb " + addr;
        return "memory read --size 1 --count " + std::to_string(count) + " " + addr;
    }

    std::string write_reg_cmd(const std::string& reg, const std::string& value) const {
        if (name == "gdb") return "set $" + reg + " = " + value;
        return "register write " + reg + " " + value;
    }

    std::string radix_cmd(bool hex) const {
        if (name == "gdb") return hex ? "set output-radix 16" : "set output-radix 10";
        return hex ? "settings set --exists target.display-hex-immediate true"
                   : "settings set --exists target.display-hex-immediate false";
    }

    std::string expand_local_cmd(const std::string& var) const {
        if (this->name == "gdb") return "p " + var;
        return "frame variable " + var + " --ptr-depth 2 --show-types";
    }

    std::string breakpoint(const std::string& spec, const std::string& condition = {},
                           const std::string& thread = {}) const {
        auto pos = spec.rfind(':');
        bool file_line = pos != std::string::npos && pos > 0 &&
                         spec.find_first_not_of("0123456789", pos + 1) == std::string::npos;
        std::string cmd;
        if (name == "gdb") {
            cmd = "break " + spec;
            if (!thread.empty()) cmd += " thread " + thread;
            if (!condition.empty()) cmd += " if " + condition;
            return cmd;
        }
        if (file_line) {
            cmd = "breakpoint set --file \"" + spec.substr(0, pos) +
                  "\" --line " + spec.substr(pos + 1);
        } else {
            cmd = "breakpoint set --name " + spec;
        }
        if (!thread.empty()) cmd += " --thread-index " + thread;
        if (!condition.empty()) cmd += " --condition \"" + condition + "\"";
        return cmd;
    }

    std::vector<std::string> load_target(const std::string& exe) const {
        std::vector<std::string> cmds{"file \"" + exe + "\""};
        if (name == "lldb") cmds.emplace_back("platform select host");
        return cmds;
    }

    static std::vector<std::string> parse_env(const std::string& text) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : text) {
            if (c == ',' || c == '\n') {
                while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) cur.pop_back();
                auto s = cur.find_first_not_of(" \t");
                if (s != std::string::npos && cur.find('=') != std::string::npos)
                    out.push_back(cur.substr(s));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        auto s = cur.find_first_not_of(" \t");
        if (s != std::string::npos && cur.find('=') != std::string::npos)
            out.push_back(cur.substr(s));
        return out;
    }

    std::vector<std::string> apply_params(const std::string& args, const std::string& cwd,
                                          const std::string& env, const std::string& extra,
                                          bool stop_at_entry) const {
        std::vector<std::string> cmds;
        if (name == "gdb") {
            if (!cwd.empty()) cmds.push_back("cd \"" + cwd + "\"");
            cmds.push_back(args.empty() ? "set args" : "set args " + args);
            for (auto& p : parse_env(env)) cmds.push_back("set environment " + p);
        } else {
            if (!cwd.empty()) {
                cmds.push_back("settings set --exists target.exec-search-paths \"" + cwd + "\"");
                cmds.push_back("platform settings -w \"" + cwd + "\"");
                cmds.push_back("settings set --exists target.launch-working-dir \"" + cwd + "\"");
            }
            if (!args.empty())
                cmds.push_back("settings set --exists target.run-args " + args);
            else
                cmds.emplace_back("settings clear target.run-args");
            (void)stop_at_entry;
            for (auto& p : parse_env(env)) {
                auto eq = p.find('=');
                cmds.push_back("settings set --exists target.env-vars " + p.substr(0, eq) + "=\"" +
                               p.substr(eq + 1) + "\"");
            }
        }
        std::istringstream in(extra);
        std::string line;
        while (std::getline(in, line)) {
            auto a = line.find_first_not_of(" \t");
            if (a == std::string::npos || line[a] == '#') continue;
            cmds.push_back(line.substr(a));
        }
        return cmds;
    }

    std::string run_cmd(bool stop_at_entry) const {
        if (name == "gdb") return stop_at_entry ? "start" : "run";
        return stop_at_entry ? "process launch --stop-at-entry" : "process launch";
    }

    std::vector<std::string> io_cmds(const std::string& stdin_path, const std::string& stdout_path,
                                     const std::string& stderr_path, const std::string& tty,
                                     const std::string& args = {}) const {
        std::vector<std::string> cmds;
        if (name == "gdb") {
            if (!tty.empty()) cmds.push_back("tty " + tty);
            std::string redir;
            if (!stdin_path.empty()) redir += " < \"" + stdin_path + "\"";
            if (!stdout_path.empty()) redir += " > \"" + stdout_path + "\"";
            if (!stderr_path.empty()) redir += " 2> \"" + stderr_path + "\"";
            if (!redir.empty() || !args.empty())
                cmds.push_back(std::string("set args ") + args + redir);
        } else {
            if (!stdin_path.empty())
                cmds.push_back("settings set --exists target.input-path \"" + stdin_path + "\"");
            if (!stdout_path.empty())
                cmds.push_back("settings set --exists target.output-path \"" + stdout_path + "\"");
            if (!stderr_path.empty())
                cmds.push_back("settings set --exists target.error-path \"" + stderr_path + "\"");
            if (!tty.empty()) {
                cmds.push_back("settings set --exists target.input-path \"" + tty + "\"");
                cmds.push_back("settings set --exists target.output-path \"" + tty + "\"");
                cmds.push_back("settings set --exists target.error-path \"" + tty + "\"");
            }
        }
        return cmds;
    }

    std::vector<std::string> fork_cmds(const std::string& follow, bool detach_on_fork) const {
        std::vector<std::string> cmds;
        std::string mode = follow.empty() ? "parent" : follow;
        if (name == "gdb") {
            cmds.push_back("set follow-fork-mode " + mode);
            cmds.push_back(std::string("set detach-on-fork ") + (detach_on_fork ? "on" : "off"));
            cmds.push_back("set follow-exec-mode new");
        } else {
            if (mode == "ask") mode = "parent";
            cmds.push_back("settings set --exists target.process.follow-fork-mode " + mode);
            cmds.push_back(std::string("settings set --exists target.process.stop-on-fork ") +
                           (detach_on_fork ? "false" : "true"));
        }
        return cmds;
    }

    std::string remote_cmd(const std::string& spec) const {
        if (name == "gdb") return "target remote " + spec;
        return "gdb-remote " + spec;
    }

    std::string core_cmd(const std::string& path, const std::string& exe) const {
        if (name == "gdb") return "core-file \"" + path + "\"";
        if (!exe.empty()) return "target create --core \"" + path + "\" \"" + exe + "\"";
        return "target create --core \"" + path + "\"";
    }

    std::vector<std::string> reload_cmds(const std::string& exe) const {
        std::vector<std::string> cmds;
        if (name == "gdb") {
            cmds.push_back("file \"" + exe + "\"");
            cmds.push_back("symbol-file \"" + exe + "\"");
        } else {
            cmds.push_back("target modules load --file \"" + exe + "\" --slide 0");
            cmds.push_back("file \"" + exe + "\"");
        }
        return cmds;
    }

    std::vector<std::string> discover_source_cmds() const {
        if (name == "gdb")
            return {"info line main", "info sources", "info functions main"};
        return {"image lookup -v -n main", "target modules dump symfile"};
    }

    std::string source_map_cmd(const std::string& from, const std::string& to) const {
        if (name == "gdb") return "set substitute-path " + from + " " + to;
        return "settings append target.source-map " + from + " " + to;
    }

    std::string select_frame(int n) const {
        std::string s = frame_select;
        auto p = s.find("{n}");
        if (p != std::string::npos) s.replace(p, 3, std::to_string(n));
        return s;
    }
};

inline std::string which_bin(const std::vector<std::string>& names) {
    const char* path = std::getenv("PATH");
    if (!path) return names.empty() ? "" : names.front();
    std::string paths = path;
    std::string item;
    for (const auto& name : names) {
        std::istringstream in(paths);
        while (std::getline(in, item, ':')) {
            std::string cand = item + "/" + name;
            if (access(cand.c_str(), X_OK) == 0) return cand;
        }
    }
    return names.empty() ? "" : names.front();
}

inline Backend make_lldb(const std::string& override_bin = {}) {
    Backend b;
    b.name = "lldb";
    b.binary = override_bin.empty()
                   ? which_bin({"lldb-22", "lldb-21", "lldb-20", "lldb-19", "lldb-18", "lldb"})
                   : override_bin;
    b.prompt = "(lldb)";
    b.argv = {b.binary, "--no-use-colors",
              "-o", "settings set use-color false",
              "-o", "settings set prompt '(lldb) '",
              "-o", "settings set stop-line-count-before 0",
              "-o", "settings set stop-line-count-after 0",
              "-o", "settings set stop-disassembly-count 0",
              "-o", "settings set stop-disassembly-display never",
              "-o", "settings set auto-confirm true"};
    b.continue_cmd = "process continue";
    b.step_in = "thread step-in";
    b.step_over = "thread step-over";
    b.step_out = "thread step-out";
    b.kill_cmd = "process kill";
    b.threads_cmd = "thread list";
    b.locals_cmd = "frame variable";
    b.regs_cmd = "register read";
    b.breaks_cmd = "breakpoint list";
    b.frame_info_cmd = "frame info";
    b.frame_select = "frame select {n}";
    b.thread_select = "thread select {n}";
    b.delete_breaks = "breakpoint delete --force";
    b.disasm_cmd = "disassemble --frame";
    return b;
}

inline Backend make_gdb(const std::string& override_bin = {}) {
    Backend b;
    b.name = "gdb";
    b.binary = override_bin.empty() ? which_bin({"gdb"}) : override_bin;
    b.prompt = "(gdb)";
    b.argv = {b.binary, "-q", "--nh", "--interpreter=mi2",
              "-ex", "set pagination off",
              "-ex", "set confirm off",
              "-ex", "set print pretty on",
              "-ex", "set disassemble-next-line off"};
    b.continue_cmd = "continue";
    b.step_in = "step";
    b.step_over = "next";
    b.step_out = "finish";
    b.kill_cmd = "kill";
    b.threads_cmd = "info threads";
    b.locals_cmd = "info locals";
    b.regs_cmd = "info registers";
    b.breaks_cmd = "info breakpoints";
    b.frame_info_cmd = "info frame";
    b.frame_select = "frame {n}";
    b.thread_select = "thread {n}";
    b.delete_breaks = "delete breakpoints";
    b.disasm_cmd = "disassemble";
    return b;
}

struct Config {
    std::string debugger = "lldb";
    std::string target;
    std::string args;
    std::string cwd;
    std::string env;
    std::string extra;
    bool stop_at_entry = false;
    std::string debugger_path;
    std::vector<std::string> breakpoints;
    std::vector<std::string> sources;
    bool load = false;
    bool run = false;
    int attach = 0;
    std::string core;
    std::string stdin_path;
    std::string stdout_path;
    std::string stderr_path;
    std::string tty;
    std::string remote;
    std::string follow_fork = "parent";
    bool detach_on_fork = true;
    bool target_console = true;
    bool target_bind_stdin = true;
    bool target_bind_stdout = true;
    bool target_bind_stderr = true;
    bool target_echo = true;
    bool target_crlf = true;
    std::string source_map;
    int font_size = 12;
    std::string theme = "dark";
    int win_w = 1280;
    int win_h = 820;
    bool hex_locals = false;
    bool no_save = false;
    std::string config_path;
};

inline std::string sdbg_home_config_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::string(home) + "/.config/sdbg";
}

inline std::string resolve_config_path(const std::string& explicit_path = {}) {
    if (!explicit_path.empty()) return explicit_path;
    const std::string name = "debugger.json";
    std::vector<std::string> cands{name, std::string("../") + name};
    auto home = sdbg_home_config_dir();
    if (!home.empty()) cands.push_back(home + "/" + name);
    for (const auto& p : cands) {
        if (access(p.c_str(), R_OK) == 0) return p;
    }
    if (access(".", W_OK) == 0) return name;
    if (!home.empty()) return home + "/" + name;
    return name;
}

inline void ensure_parent_dir(const std::string& path) {
    auto slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return;
    auto dir = path.substr(0, slash);
    mkdir(dir.c_str(), 0755);
    auto slash2 = dir.rfind('/');
    if (slash2 != std::string::npos)
        mkdir(dir.substr(0, slash2).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
}

inline std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(c);
        } else if (c == '\n')
            o += "\\n";
        else
            o.push_back(c);
    }
    return o;
}

inline std::string json_get(const std::string& src, const std::string& key) {
    auto k = "\"" + key + "\"";
    auto p = src.find(k);
    if (p == std::string::npos) return {};
    p = src.find(':', p);
    if (p == std::string::npos) return {};
    p = src.find_first_not_of(" \t\n", p + 1);
    if (p == std::string::npos) return {};
    if (src[p] == '"') {
        std::string o;
        for (size_t i = p + 1; i < src.size(); ++i) {
            if (src[i] == '\\' && i + 1 < src.size()) {
                o.push_back(src[i + 1] == 'n' ? '\n' : src[i + 1]);
                ++i;
            } else if (src[i] == '"')
                break;
            else
                o.push_back(src[i]);
        }
        return o;
    }
    if (src.compare(p, 4, "true") == 0) return "true";
    if (src.compare(p, 5, "false") == 0) return "false";
    auto e = src.find_first_of(",}\n", p);
    return src.substr(p, e - p);
}

inline void load_config_file(Config& cfg, const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto src = ss.str();
    auto d = json_get(src, "debugger");
    if (d == "gdb" || d == "lldb") cfg.debugger = d;
    auto t = json_get(src, "target");
    if (!t.empty()) cfg.target = t;
    auto a = json_get(src, "args");
    if (!a.empty()) cfg.args = a;
    auto c = json_get(src, "cwd");
    if (!c.empty()) cfg.cwd = c;
    auto e = json_get(src, "env");
    if (!e.empty()) cfg.env = e;
    auto x = json_get(src, "extra");
    if (!x.empty()) cfg.extra = x;
    if (json_get(src, "stop_at_entry") == "true") cfg.stop_at_entry = true;
    auto v = json_get(src, "stdin");
    if (!v.empty()) cfg.stdin_path = v;
    v = json_get(src, "stdout");
    if (!v.empty()) cfg.stdout_path = v;
    v = json_get(src, "stderr");
    if (!v.empty()) cfg.stderr_path = v;
    v = json_get(src, "tty");
    if (!v.empty()) cfg.tty = v;
    v = json_get(src, "remote");
    if (!v.empty()) cfg.remote = v;
    v = json_get(src, "follow_fork");
    if (!v.empty()) cfg.follow_fork = v;
    if (json_get(src, "detach_on_fork") == "false") cfg.detach_on_fork = false;
    if (json_get(src, "target_console") == "false") cfg.target_console = false;
    if (json_get(src, "target_bind_stdin") == "false") cfg.target_bind_stdin = false;
    if (json_get(src, "target_bind_stdout") == "false") cfg.target_bind_stdout = false;
    if (json_get(src, "target_bind_stderr") == "false") cfg.target_bind_stderr = false;
    if (json_get(src, "target_echo") == "false") cfg.target_echo = false;
    if (json_get(src, "target_crlf") == "false") cfg.target_crlf = false;
    v = json_get(src, "source_map");
    if (!v.empty()) cfg.source_map = v;
    v = json_get(src, "font_size");
    if (!v.empty()) cfg.font_size = std::atoi(v.c_str());
    v = json_get(src, "theme");
    if (!v.empty()) cfg.theme = v;
    v = json_get(src, "win_w");
    if (!v.empty()) cfg.win_w = std::atoi(v.c_str());
    v = json_get(src, "win_h");
    if (!v.empty()) cfg.win_h = std::atoi(v.c_str());
    if (json_get(src, "hex_locals") == "true") cfg.hex_locals = true;
}

inline void save_config_file(const Config& cfg, const std::string& path) {
    if (cfg.no_save) return;
    ensure_parent_dir(path);
    std::ofstream out(path);
    if (!out) return;
    out << "{\n"
        << "  \"debugger\": \"" << json_escape(cfg.debugger) << "\",\n"
        << "  \"target\": \"" << json_escape(cfg.target) << "\",\n"
        << "  \"args\": \"" << json_escape(cfg.args) << "\",\n"
        << "  \"cwd\": \"" << json_escape(cfg.cwd) << "\",\n"
        << "  \"env\": \"" << json_escape(cfg.env) << "\",\n"
        << "  \"extra\": \"" << json_escape(cfg.extra) << "\",\n"
        << "  \"stop_at_entry\": " << (cfg.stop_at_entry ? "true" : "false") << ",\n"
        << "  \"stdin\": \"" << json_escape(cfg.stdin_path) << "\",\n"
        << "  \"stdout\": \"" << json_escape(cfg.stdout_path) << "\",\n"
        << "  \"stderr\": \"" << json_escape(cfg.stderr_path) << "\",\n"
        << "  \"tty\": \"" << json_escape(cfg.tty) << "\",\n"
        << "  \"remote\": \"" << json_escape(cfg.remote) << "\",\n"
        << "  \"follow_fork\": \"" << json_escape(cfg.follow_fork) << "\",\n"
        << "  \"detach_on_fork\": " << (cfg.detach_on_fork ? "true" : "false") << ",\n"
        << "  \"target_console\": " << (cfg.target_console ? "true" : "false") << ",\n"
        << "  \"target_bind_stdin\": " << (cfg.target_bind_stdin ? "true" : "false") << ",\n"
        << "  \"target_bind_stdout\": " << (cfg.target_bind_stdout ? "true" : "false") << ",\n"
        << "  \"target_bind_stderr\": " << (cfg.target_bind_stderr ? "true" : "false") << ",\n"
        << "  \"target_echo\": " << (cfg.target_echo ? "true" : "false") << ",\n"
        << "  \"target_crlf\": " << (cfg.target_crlf ? "true" : "false") << ",\n"
        << "  \"source_map\": \"" << json_escape(cfg.source_map) << "\",\n"
        << "  \"font_size\": " << cfg.font_size << ",\n"
        << "  \"theme\": \"" << json_escape(cfg.theme) << "\",\n"
        << "  \"win_w\": " << cfg.win_w << ",\n"
        << "  \"win_h\": " << cfg.win_h << ",\n"
        << "  \"hex_locals\": " << (cfg.hex_locals ? "true" : "false") << "\n"
        << "}\n";
}

inline void print_help(const char* argv0) {
    std::printf(
        "Usage: %s [options] [target] [-- target-args...]\n\n"
        "  -d, --debugger lldb|gdb   Engine\n"
        "      --lldb / --gdb        Shorthand\n"
        "      --debugger-path PATH  Binary override\n"
        "  -c, --cwd DIR             Working directory\n"
        "  -a, --args STRING         Target argv\n"
        "  -e, --env KEY=VAL         Environment (repeatable)\n"
        "  -s, --setup CMD           Debugger setup command\n"
        "  -b, --break SPEC          Breakpoint symbol or file:line\n"
        "      --stop-at-entry       Stop before main\n"
        "      --load / --run        Load or run after start\n"
        "      --attach PID          Attach to process\n"
        "      --core FILE           Load core dump\n"
        "      --stdin FILE          Inferior stdin redirect\n"
        "      --stdout FILE         Inferior stdout redirect\n"
        "      --stderr FILE         Inferior stderr redirect\n"
        "      --tty PATH            Inferior controlling TTY\n"
        "      --remote HOST:PORT    Connect to gdbserver / gdb-remote\n"
        "      --follow-fork MODE    parent|child|ask|both\n"
        "      --no-target-console   Do not allocate an inferior PTY\n"
        "      --target-echo / --no-target-echo\n"
        "      --source-map FROM=TO  Rewrite compile paths to local paths\n"
        "      --source FILE         Open source tab\n"
        "      --config FILE         Config JSON path\n"
        "      --no-save-config\n"
        "      --help / --version\n",
        argv0);
}

inline Config parse_cli(int argc, char** argv) {
    Config cfg;
    cfg.cwd = ".";
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) cfg.cwd = cwd;
    std::vector<std::string> pos;
    bool after_dash = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!after_dash && a == "--") {
            after_dash = true;
            continue;
        }
        if (!after_dash && (a == "-h" || a == "--help")) {
            print_help(argv[0]);
            std::exit(0);
        }
        if (!after_dash && a == "--version") {
            std::puts("debugger-gui-cpp 1.0");
            std::exit(0);
        }
        if (!after_dash && (a == "-d" || a == "--debugger"))
            cfg.debugger = need(a.c_str());
        else if (!after_dash && a == "--lldb")
            cfg.debugger = "lldb";
        else if (!after_dash && a == "--gdb")
            cfg.debugger = "gdb";
        else if (!after_dash && a == "--debugger-path")
            cfg.debugger_path = need(a.c_str());
        else if (!after_dash && (a == "-c" || a == "--cwd"))
            cfg.cwd = need(a.c_str());
        else if (!after_dash && (a == "-a" || a == "--args"))
            cfg.args = need(a.c_str());
        else if (!after_dash && (a == "-e" || a == "--env")) {
            if (!cfg.env.empty()) cfg.env += "\n";
            cfg.env += need(a.c_str());
        } else if (!after_dash && (a == "-s" || a == "--setup")) {
            if (!cfg.extra.empty()) cfg.extra += "\n";
            cfg.extra += need(a.c_str());
        } else if (!after_dash && (a == "-b" || a == "--break"))
            cfg.breakpoints.push_back(need(a.c_str()));
        else if (!after_dash && a == "--stop-at-entry")
            cfg.stop_at_entry = true;
        else if (!after_dash && a == "--load")
            cfg.load = true;
        else if (!after_dash && a == "--run") {
            cfg.run = true;
            cfg.load = true;
        } else if (!after_dash && a == "--attach")
            cfg.attach = std::atoi(need(a.c_str()).c_str());
        else if (!after_dash && a == "--core")
            cfg.core = need(a.c_str());
        else if (!after_dash && a == "--stdin")
            cfg.stdin_path = need(a.c_str());
        else if (!after_dash && a == "--stdout")
            cfg.stdout_path = need(a.c_str());
        else if (!after_dash && a == "--stderr")
            cfg.stderr_path = need(a.c_str());
        else if (!after_dash && a == "--tty")
            cfg.tty = need(a.c_str());
        else if (!after_dash && a == "--remote")
            cfg.remote = need(a.c_str());
        else if (!after_dash && a == "--follow-fork")
            cfg.follow_fork = need(a.c_str());
        else if (!after_dash && a == "--no-target-console")
            cfg.target_console = false;
        else if (!after_dash && a == "--target-echo")
            cfg.target_echo = true;
        else if (!after_dash && a == "--no-target-echo")
            cfg.target_echo = false;
        else if (!after_dash && a == "--source-map") {
            if (!cfg.source_map.empty()) cfg.source_map += "\n";
            cfg.source_map += need(a.c_str());
        } else if (!after_dash && a == "--source")
            cfg.sources.push_back(need(a.c_str()));
        else if (!after_dash && a == "--config")
            cfg.config_path = need(a.c_str());
        else if (!after_dash && a == "--no-save-config")
            cfg.no_save = true;
        else if (!after_dash && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            std::exit(2);
        } else {
            pos.push_back(a);
        }
    }
    if (!pos.empty() && cfg.target.empty()) {
        cfg.target = pos.front();
        if (cfg.args.empty()) {
            for (size_t i = 1; i < pos.size(); ++i) {
                if (!cfg.args.empty()) cfg.args += " ";
                cfg.args += pos[i];
            }
        }
    } else if (cfg.args.empty() && pos.size() > 1) {
        for (size_t i = 1; i < pos.size(); ++i) {
            if (!cfg.args.empty()) cfg.args += " ";
            cfg.args += pos[i];
        }
    }
    if (cfg.debugger != "gdb" && cfg.debugger != "lldb") cfg.debugger = "lldb";
    return cfg;
}
