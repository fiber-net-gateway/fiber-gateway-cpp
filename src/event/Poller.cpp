#include "Poller.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>
#include <unistd.h>

namespace fiber::event {

namespace {

constexpr std::uint32_t to_mask(Poller::Event events) { return static_cast<std::uint32_t>(events); }

constexpr std::uint32_t to_mask(Poller::Mode mode) { return static_cast<std::uint32_t>(mode); }

std::uint32_t to_epoll_events(Poller::Event events, Poller::Mode mode) {
    std::uint32_t mask = 0;
    auto bits = to_mask(events);
    if (bits & to_mask(Poller::Event::Read)) {
        mask |= EPOLLIN;
    }
    if (bits & to_mask(Poller::Event::Write)) {
        mask |= EPOLLOUT;
    }
    if (mask) {
        mask |= EPOLLERR | EPOLLHUP;
        auto mode_bits = to_mask(mode);
        if (mode_bits & to_mask(Poller::Mode::Edge)) {
            mask |= EPOLLET;
        }
        if (mode_bits & to_mask(Poller::Mode::OneShot)) {
            mask |= EPOLLONESHOT;
        }
    }
    return mask;
}

} // namespace

Poller::Poller() { epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC); }

Poller::~Poller() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool Poller::valid() const { return epoll_fd_ >= 0; }

fiber::common::IoErr Poller::add(int fd, Event events, Item *item, Mode mode) {
    if (item) {
        item->fd_ = fd;
        item->interested_ = events;
    }
    epoll_event ev{};
    ev.events = to_epoll_events(events, mode);
    ev.data.ptr = item;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0) {
        return fiber::common::IoErr::None;
    }
    return fiber::common::io_err_from_errno(errno);
}

fiber::common::IoErr Poller::mod(int fd, Event events, Item *item, Mode mode) {
    if (item) {
        item->fd_ = fd;
        item->interested_ = events;
    }
    epoll_event ev{};
    ev.events = to_epoll_events(events, mode);
    ev.data.ptr = item;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
        return fiber::common::IoErr::None;
    }
    return fiber::common::io_err_from_errno(errno);
}

fiber::common::IoErr Poller::del(int fd) {
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0) {
        return fiber::common::IoErr::None;
    }
    return fiber::common::io_err_from_errno(errno);
}

int Poller::wait(epoll_event *events, int max_events, std::chrono::nanoseconds timeout) {
    const bool infinite = timeout == std::chrono::nanoseconds::max();
    timespec timeout_spec{};
    const timespec *timeout_ptr = nullptr;
    if (!infinite) {
        timeout = std::max(timeout, std::chrono::nanoseconds::zero());
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
        const auto nanoseconds = timeout - seconds;
        timeout_spec.tv_sec = static_cast<time_t>(seconds.count());
        timeout_spec.tv_nsec = static_cast<long>(nanoseconds.count());
        timeout_ptr = &timeout_spec;
    }

    int result = ::epoll_pwait2(epoll_fd_, events, max_events, timeout_ptr, nullptr);
    if (result >= 0 || errno != ENOSYS) {
        return result;
    }

    int timeout_ms = -1;
    if (!infinite) {
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
        if (std::chrono::duration_cast<std::chrono::nanoseconds>(milliseconds) < timeout) {
            ++milliseconds;
        }
        timeout_ms = milliseconds.count() > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                            : static_cast<int>(milliseconds.count());
    }
    return ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);
}

} // namespace fiber::event
