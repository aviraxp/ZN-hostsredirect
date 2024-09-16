#pragma once
#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>
#include <iostream>

#if defined(__LP64__)
# define LP_SELECT(lp32, lp64) lp64
#else
# define LP_SELECT(lp32, lp64) lp32
#endif

class UniqueFd {
    using Fd = int;
public:
    UniqueFd() = default;

    inline UniqueFd(Fd fd) : fd_(fd) {}

    inline ~UniqueFd() { if (fd_ >= 0) close(fd_); }

    // Disallow copy
    inline UniqueFd(const UniqueFd&) = delete;

    inline UniqueFd& operator=(const UniqueFd&) = delete;

    // Allow move
    inline UniqueFd(UniqueFd&& other) { std::swap(fd_, other.fd_); }

    inline UniqueFd& operator=(UniqueFd&& other) {
        std::swap(fd_, other.fd_);
        return *this;
    }

    inline void drop() {
        close(fd_);
        fd_ = -1;
    }

    inline int into_fd() {
        int r = -1;
        std::swap(r, fd_);
        return r;
    }

    inline int as_fd() {
        return fd_;
    }

    // Implict cast to Fd
    inline operator const Fd&() const { return fd_; }

private:
    Fd fd_ = -1;
};
