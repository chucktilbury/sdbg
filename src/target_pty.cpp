#include "target_pty.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

TargetPty::TargetPty(DataFn on_data) : on_data_(std::move(on_data)) {}

TargetPty::~TargetPty() { stop(); }

bool TargetPty::start() {
    stop();
    last_error_.clear();
    master_ = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_ < 0) {
        last_error_ = std::string("posix_openpt: ") + std::strerror(errno);
        return false;
    }
    if (grantpt(master_) != 0) {
        last_error_ = std::string("grantpt: ") + std::strerror(errno);
        close(master_);
        master_ = -1;
        return false;
    }
    if (unlockpt(master_) != 0) {
        last_error_ = std::string("unlockpt: ") + std::strerror(errno);
        close(master_);
        master_ = -1;
        return false;
    }
    char buf[128]{};
#if defined(_GNU_SOURCE) || defined(__linux__)
    if (ptsname_r(master_, buf, sizeof(buf)) != 0) {
        last_error_ = std::string("ptsname_r: ") + std::strerror(errno);
        close(master_);
        master_ = -1;
        return false;
    }
    slave_name_ = buf;
#else
    char* name = ptsname(master_);
    if (!name) {
        last_error_ = std::string("ptsname: ") + std::strerror(errno);
        close(master_);
        master_ = -1;
        return false;
    }
    slave_name_ = name;
#endif
    int slave = open(slave_name_.c_str(), O_RDWR | O_NOCTTY);
    if (slave < 0) {
        last_error_ = "open " + slave_name_ + ": " + std::strerror(errno);
        close(master_);
        master_ = -1;
        slave_name_.clear();
        return false;
    }
    termios tio{};
    if (tcgetattr(slave, &tio) == 0) {
        tio.c_lflag |= ECHO;
        tio.c_oflag |= OPOST | ONLCR;
        tcsetattr(slave, TCSANOW, &tio);
    }
    close(slave);
    int flags = fcntl(master_, F_GETFL, 0);
    fcntl(master_, F_SETFL, flags | O_NONBLOCK);
    alive_ = true;
    reader_ = std::thread([this] { reader(); });
    return true;
}

void TargetPty::stop() {
    alive_ = false;
    if (master_ >= 0) {
        close(master_);
        master_ = -1;
    }
    if (reader_.joinable()) reader_.join();
    slave_name_.clear();
}

void TargetPty::write(const std::string& data) {
    std::lock_guard<std::mutex> lock(mu_);
    if (master_ < 0 || data.empty()) return;
    const char* p = data.data();
    size_t left = data.size();
    while (left) {
        ssize_t n = ::write(master_, p, left);
        if (n < 0) break;
        p += n;
        left -= static_cast<size_t>(n);
    }
}

void TargetPty::reader() {
    while (alive_ && master_ >= 0) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(master_, &rf);
        timeval tv{0, 200000};
        int rc = select(master_ + 1, &rf, nullptr, nullptr, &tv);
        if (rc <= 0) continue;
        char buf[4096];
        ssize_t n = ::read(master_, buf, sizeof(buf));
        if (n > 0 && on_data_) on_data_(std::string(buf, buf + n));
        if (n == 0) break;
    }
}
