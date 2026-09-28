#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class TargetPty {
public:
    using DataFn = std::function<void(const std::string&)>;

    explicit TargetPty(DataFn on_data);
    ~TargetPty();

    bool start();
    void stop();
    bool alive() const { return alive_; }
    const std::string& slave_name() const { return slave_name_; }
    const std::string& last_error() const { return last_error_; }
    void write(const std::string& data);

private:
    void reader();

    DataFn on_data_;
    int master_ = -1;
    std::string slave_name_;
    std::string last_error_;
    std::atomic<bool> alive_{false};
    std::thread reader_;
    std::mutex mu_;
};
