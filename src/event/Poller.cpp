#include <fiber/event/Poller.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "fiber/FiberPlatformConfig.h"

namespace fiber::event {

namespace {

constexpr std::uint32_t to_mask(Poller::Event events) { return static_cast<std::uint32_t>(events); }

constexpr std::uint32_t to_mask(Poller::Mode mode) { return static_cast<std::uint32_t>(mode); }

struct KernelTimespec {
    std::int64_t tv_sec = 0;
    std::int64_t tv_nsec = 0;
};

const KernelTimespec *make_timeout(std::chrono::steady_clock::time_point deadline, KernelTimespec &timeout_spec) {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return nullptr;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto timeout = deadline <= now ? std::chrono::nanoseconds::zero()
                                         : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    timeout_spec.tv_sec = seconds.count();
    timeout_spec.tv_nsec = (timeout - seconds).count();
    return &timeout_spec;
}

std::uint32_t to_epoll_events(Poller::Event events, Poller::Mode mode) {
    std::uint32_t mask = 0;
    auto bits = to_mask(events);
    if (bits & to_mask(Poller::Event::Read)) {
        mask |= EPOLLIN;
    }
    if (bits & to_mask(Poller::Event::Write)) {
        mask |= EPOLLOUT;
    }
    if (bits & to_mask(Poller::Event::Terminal)) {
        mask |= EPOLLERR | EPOLLHUP;
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

Poller::Poller() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        return;
    }

#if FIBER_FORCE_TIMERFD_POLLER || !FIBER_HAVE_EPOLL_PWAIT2_SYSCALL
    if (init_timer_fd() != 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }
    wait_backend_ = WaitBackend::TimerFd;
#endif
}

Poller::~Poller() {
    if (timer_fd_ >= 0) {
        ::close(timer_fd_);
    }
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

int Poller::wait(epoll_event *events, int max_events, std::chrono::steady_clock::time_point deadline) {
#if FIBER_HAVE_EPOLL_PWAIT2_SYSCALL
    if (wait_backend_ != WaitBackend::TimerFd) {
        KernelTimespec timeout_spec{};
        const KernelTimespec *timeout_ptr = make_timeout(deadline, timeout_spec);
        int result = static_cast<int>(::syscall(SYS_epoll_pwait2, epoll_fd_, events, max_events, timeout_ptr, nullptr,
                                                static_cast<std::size_t>(_NSIG / 8)));
        if (result >= 0 || errno != ENOSYS) {
            wait_backend_ = WaitBackend::EpollPwait2;
            return result;
        }
        if (init_timer_fd() != 0) {
            return -1;
        }
        wait_backend_ = WaitBackend::TimerFd;
    }
#endif

    return wait_timer_fd(events, max_events, deadline);
}

int Poller::init_timer_fd() {
    if (timer_fd_ >= 0) {
        return 0;
    }

    const int timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        return -1;
    }

    epoll_event timer_event{};
    timer_event.events = EPOLLIN;
    timer_event.data.ptr = &timer_fd_tag_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd, &timer_event) != 0) {
        const int saved_errno = errno;
        ::close(timer_fd);
        errno = saved_errno;
        return -1;
    }

    timer_fd_ = timer_fd;
    return 0;
}

int Poller::wait_timer_fd(epoll_event *events, int max_events, std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline != std::chrono::steady_clock::time_point::max() && deadline <= now) {
        return wait_epoll(events, max_events, 0);
    }
    if (sync_timer_fd(deadline, now) != 0) {
        return -1;
    }
    return wait_epoll(events, max_events, -1);
}

int Poller::wait_epoll(epoll_event *events, int max_events, int timeout_ms) {
    const int count = ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);
    if (count <= 0) {
        return count;
    }

    int output_count = 0;
    for (int i = 0; i < count; ++i) {
        if (events[i].data.ptr == &timer_fd_tag_) {
            if (drain_timer_fd() != 0) {
                return -1;
            }
            armed_deadline_ = std::chrono::steady_clock::time_point::max();
            continue;
        }
        if (output_count != i) {
            events[output_count] = events[i];
        }
        ++output_count;
    }
    return output_count;
}

int Poller::sync_timer_fd(std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now) {
    if (deadline == armed_deadline_) {
        return 0;
    }

    itimerspec timer_spec{};
    if (deadline != std::chrono::steady_clock::time_point::max()) {
        auto timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
        timeout = std::max(timeout, std::chrono::nanoseconds{1});
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
        timer_spec.it_value.tv_sec = static_cast<time_t>(seconds.count());
        timer_spec.it_value.tv_nsec = static_cast<long>((timeout - seconds).count());
    }
    if (::timerfd_settime(timer_fd_, 0, &timer_spec, nullptr) != 0) {
        return -1;
    }
    armed_deadline_ = deadline;
    return 0;
}

int Poller::drain_timer_fd() {
    std::uint64_t expirations = 0;
    for (;;) {
        const ssize_t result = ::read(timer_fd_, &expirations, sizeof(expirations));
        if (result == static_cast<ssize_t>(sizeof(expirations))) {
            return 0;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && errno == EAGAIN) {
            return 0;
        }
        if (result >= 0) {
            errno = EIO;
        }
        return -1;
    }
}

} // namespace fiber::event
