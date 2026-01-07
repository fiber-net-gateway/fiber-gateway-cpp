#include "Poller.h"

#include <unistd.h>

namespace fiber::event {

namespace {

constexpr std::uint32_t to_mask(Poller::Event events) {
    return static_cast<std::uint32_t>(events);
}

std::uint32_t to_epoll_events(Poller::Event events) {
    std::uint32_t mask = 0;
    auto bits = to_mask(events);
    if (bits & to_mask(Poller::Event::Read)) {
        mask |= EPOLLIN;
    }
    if (bits & to_mask(Poller::Event::Write)) {
        mask |= EPOLLOUT;
    }
    mask |= EPOLLERR | EPOLLHUP;
    return mask;
}

} // namespace

Poller::Poller() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
}

Poller::~Poller() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool Poller::valid() const {
    return epoll_fd_ >= 0;
}

int Poller::add(int fd, Event events, Item *item) {
    if (item) {
        item->fd_ = fd;
    }
    epoll_event ev{};
    ev.events = to_epoll_events(events);
    ev.data.ptr = item;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

int Poller::mod(int fd, Event events, Item *item) {
    if (item) {
        item->fd_ = fd;
    }
    epoll_event ev{};
    ev.events = to_epoll_events(events);
    ev.data.ptr = item;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

int Poller::del(int fd) {
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

int Poller::wait(epoll_event *events, int max_events, int timeout_ms) {
    return ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);
}

} // namespace fiber::event
