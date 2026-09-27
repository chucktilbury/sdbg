#pragma once
// Full file lives in the project tree; this push continues in follow-up if truncated.
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

// NOTE: complete implementation is in artifacts; see following commit for full header.
struct Backend {
    std::string name;
    std::string binary;
    std::string prompt;
    std::vector<std::string> argv;
    std::string continue_cmd, step_in, step_over, step_out, kill_cmd;
    std::string threads_cmd, locals_cmd, regs_cmd, breaks_cmd, frame_info_cmd;
    std::string frame_select, thread_select, delete_breaks, disasm_cmd;
    std::string select_thread(int n) const;
    std::string eval_cmd(const std::string& expr) const;
    std::string attach_cmd(int pid) const;
    std::string delete_bp_id(const std::string& id) const;
    std::string enable_bp_id(const std::string& id, bool on) const;
    std::string mem_cmd(const std::string& addr, int count = 64) const;
    std::string write_reg_cmd(const std::string& reg, const std::string& value) const;
    std::string radix_cmd(bool hex) const;
    std::string expand_local_cmd(const std::string& var) const;
    std::string breakpoint(const std::string& spec, const std::string& condition = {}, const std::string& thread = {}) const;
    std::vector<std::string> load_target(const std::string& exe) const;
    static std::vector<std::string> parse_env(const std::string& text);
    std::vector<std::string> apply_params(const std::string& args, const std::string& cwd, const std::string& env, const std::string& extra, bool stop_at_entry) const;
    std::string run_cmd(bool stop_at_entry) const;
    std::vector<std::string> io_cmds(const std::string& stdin_path, const std::string& stdout_path, const std::string& stderr_path, const std::string& tty, const std::string& args = {}) const;
    std::vector<std::string> fork_cmds(const std::string& follow, bool detach_on_fork) const;
    std::string remote_cmd(const std::string& spec) const;
    std::string core_cmd(const std::string& path, const std::string& exe) const;
    std::vector<std::string> reload_cmds(const std::string& exe) const;
    std::string source_map_cmd(const std::string& from, const std::string& to) const;
    std::string select_frame(int n) const;
};

inline std::string which_bin(const std::vector<std::string>& names);
inline Backend make_lldb(const std::string& override_bin = {});
inline Backend make_gdb(const std::string& override_bin = {});
struct Config;
inline std::string resolve_config_path(const std::string& explicit_path = {});
inline void load_config_file(Config& cfg, const std::string& path);
inline void save_config_file(const Config& cfg, const std::string& path);
inline Config parse_cli(int argc, char** argv);
