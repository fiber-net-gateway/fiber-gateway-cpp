#include "EpollPoller.h"

#include <unistd.h>

namespace fiber::event {

EpollPoller::EpollPoller() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
}

EpollPoller::~EpollPoller() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool EpollPoller::valid() const {
    return epoll_fd_ >= 0;
}

int EpollPoller::add(int fd, std::uint32_t events, void *data) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = data;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

int EpollPoller::mod(int fd, std::uint32_t events, void *data) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = data;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

int EpollPoller::del(int fd) {
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

int EpollPoller::wait(epoll_event *events, int max_events, int timeout_ms) {
    return ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);
}

} // namespace fiber::event
