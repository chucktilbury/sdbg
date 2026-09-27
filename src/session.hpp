#pragma once

#include "backend.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>

class DebuggerSession {
public:
    using OutputFn = std::function<void(const std::string&)>;

    explicit DebuggerSession(Backend backend, OutputFn on_output, OutputFn on_stop = {});
    ~DebuggerSession();

    bool start();
    void stop();
    bool alive() const { return alive_ && pid_ > 0; }
    std::string command(const std::string& cmd, double timeout_s = 8.0);
    void command_async(const std::string& cmd);
    void interrupt();
    bool using_mi() const { return mi_; }
    const Backend& backend() const { return backend_; }

private:
    bool looks_idle(const std::string& buf) const;
    void drain();
    void emit(const std::string& text);
    void reader();

    void maybe_stop(const std::string& text);

    Backend backend_;
    OutputFn on_output_;
    OutputFn on_stop_;
    bool mi_ = false;
    int master_ = -1;
    pid_t pid_ = -1;
    std::atomic<bool> alive_{false};
    std::mutex mu_;
    std::thread reader_;
};
