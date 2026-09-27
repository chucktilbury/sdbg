#include "session.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdexcept>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

DebuggerSession::DebuggerSession(Backend backend, OutputFn on_output, OutputFn on_stop)
    : backend_(std::move(backend)), on_output_(std::move(on_output)), on_stop_(std::move(on_stop)) {}

DebuggerSession::~DebuggerSession() { stop(); }

bool DebuggerSession::start() {
    stop();
    int slave = -1;
    if (openpty(&master_, &slave, nullptr, nullptr, nullptr) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(master_);
        close(slave);
        master_ = -1;
        return false;
    }
    if (pid == 0) {
        close(master_);
        setsid();
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        setenv("TERM", "xterm-256color", 1);
        std::vector<char*> args;
        for (auto& s : backend_.argv) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);
        execvp(backend_.binary.c_str(), args.data());
        _exit(127);
    }
    close(slave);
    pid_ = pid;
    int flags = fcntl(master_, F_GETFL, 0);
    fcntl(master_, F_SETFL, flags | O_NONBLOCK);
    alive_ = true;
    mi_ = false;
    for (auto& a : backend_.argv)
        if (a.find("interpreter=mi") != std::string::npos) mi_ = true;
    reader_ = std::thread([this] { reader(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (backend_.name == "lldb")
        command("settings set interpreter.prompt-on-quit false");
    return true;
}

void DebuggerSession::command_async(const std::string& cmd) {
    if (!alive() || master_ < 0) return;
    std::string payload = cmd;
    if (mi_ && payload.rfind("-", 0) != 0)
        payload = "-interpreter-exec console \"" + cmd + "\"";
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    std::lock_guard<std::mutex> lock(mu_);
    ::write(master_, payload.data(), payload.size());
}

void DebuggerSession::stop() {
    alive_ = false;
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int st = 0;
        waitpid(pid_, &st, WNOHANG);
        pid_ = -1;
    }
    if (master_ >= 0) {
        close(master_);
        master_ = -1;
    }
    if (reader_.joinable()) reader_.join();
}

void DebuggerSession::interrupt() {
    if (master_ >= 0) {
        char c = 3;
        ::write(master_, &c, 1);
    }
}

void DebuggerSession::emit(const std::string& text) {
    if (text.empty()) return;
    if (on_output_) on_output_(text);
    maybe_stop(text);
}

void DebuggerSession::maybe_stop(const std::string& text) {
    if (!on_stop_) return;
    const char* keys[] = {
        "*stopped", "^stopped", "stop reason", "Breakpoint", "breakpoint hit",
        " received signal ", "Program received signal",
        "step-in", "step-over", "step-out", "Temporary breakpoint",
    };
    for (auto k : keys) {
        if (text.find(k) != std::string::npos) {
            on_stop_(text);
            return;
        }
    }
}

bool DebuggerSession::looks_idle(const std::string& buf) const {
    if (mi_) {
        if (buf.find("^running") != std::string::npos) return true;
        if (buf.find("^done") != std::string::npos) return true;
        if (buf.find("^error") != std::string::npos) return true;
    }
    auto end = buf.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return false;
    auto slice = buf.substr(0, end + 1);
    return slice.size() >= backend_.prompt.size() &&
           slice.compare(slice.size() - backend_.prompt.size(), backend_.prompt.size(),
                         backend_.prompt) == 0;
}

void DebuggerSession::drain() {
    if (master_ < 0) return;
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(master_, &rf);
        timeval tv{0, 0};
        if (select(master_ + 1, &rf, nullptr, nullptr, &tv) <= 0) break;
        char buf[65536];
        ssize_t n = ::read(master_, buf, sizeof(buf));
        if (n <= 0) break;
        emit(std::string(buf, buf + n));
    }
}

std::string DebuggerSession::command(const std::string& cmd, double timeout_s) {
    if (!alive() || master_ < 0) throw std::runtime_error("Debugger is not running");
    std::lock_guard<std::mutex> lock(mu_);
    drain();
    std::string payload = cmd;
    if (mi_ && payload.rfind("-", 0) != 0)
        payload = "-interpreter-exec console \"" + cmd + "\"";
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    ::write(master_, payload.data(), payload.size());
    std::string collected;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(static_cast<int>(timeout_s * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(master_, &rf);
        timeval tv{0, 150000};
        int rc = select(master_ + 1, &rf, nullptr, nullptr, &tv);
        if (rc > 0) {
            char buf[65536];
            ssize_t n = ::read(master_, buf, sizeof(buf));
            if (n <= 0) break;
            collected.append(buf, buf + n);
            emit(std::string(buf, buf + n));
            if (looks_idle(collected)) {
                auto extra = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
                while (std::chrono::steady_clock::now() < extra) {
                    fd_set rf2;
                    FD_ZERO(&rf2);
                    FD_SET(master_, &rf2);
                    timeval tv2{0, 50000};
                    if (select(master_ + 1, &rf2, nullptr, nullptr, &tv2) <= 0) break;
                    n = ::read(master_, buf, sizeof(buf));
                    if (n <= 0) break;
                    collected.append(buf, buf + n);
                    emit(std::string(buf, buf + n));
                }
                break;
            }
        }
    }
    return collected;
}

void DebuggerSession::reader() {
    while (alive_ && master_ >= 0) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(master_, &rf);
        timeval tv{0, 400000};
        int rc = select(master_ + 1, &rf, nullptr, nullptr, &tv);
        if (rc <= 0) {
            if (pid_ > 0) {
                int st = 0;
                pid_t w = waitpid(pid_, &st, WNOHANG);
                if (w == pid_) {
                    alive_ = false;
                    emit("\n[debugger exited]\n");
                    break;
                }
            }
            continue;
        }
        if (!mu_.try_lock()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        char buf[65536];
        ssize_t n = ::read(master_, buf, sizeof(buf));
        mu_.unlock();
        if (n > 0) emit(std::string(buf, buf + n));
    }
}
