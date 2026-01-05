#ifndef FIBER_EVENT_EPOLL_POLLER_H
#define FIBER_EVENT_EPOLL_POLLER_H

#include <cstdint>
#include <sys/epoll.h>

namespace fiber::event {

class EpollPoller {
public:
    EpollPoller();
    ~EpollPoller();

    EpollPoller(const EpollPoller &) = delete;
    EpollPoller &operator=(const EpollPoller &) = delete;
    EpollPoller(EpollPoller &&) = delete;
    EpollPoller &operator=(EpollPoller &&) = delete;

    bool valid() const;

    int add(int fd, std::uint32_t events, void *data);
    int mod(int fd, std::uint32_t events, void *data);
    int del(int fd);
    int wait(epoll_event *events, int max_events, int timeout_ms);

private:
    int epoll_fd_ = -1;
};

} // namespace fiber::event

#endif // FIBER_EVENT_EPOLL_POLLER_H
