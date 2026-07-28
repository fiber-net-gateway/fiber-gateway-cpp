#include "EventLoop.h"

#include <cerrno>
#include <cstddef>
#include <sys/eventfd.h>
#include <unistd.h>

#include "../common/Assert.h"
#include "EventLoopGroup.h"

namespace fiber::event {

thread_local EventLoop *EventLoop::current_ = nullptr;

namespace {

constexpr std::uint32_t to_mask(IoEvent events) { return static_cast<std::uint32_t>(events); }

IoEvent to_io_event(std::uint32_t events, IoEvent interested) {
    IoEvent mask = Poller::Event::None;
    if (events & (EPOLLIN | EPOLLPRI)) {
        mask |= IoEvent::Read;
    }
    if (events & EPOLLOUT) {
        mask |= IoEvent::Write;
    }
    if (events & (EPOLLERR | EPOLLHUP)) {
        mask |= interested;
    }
    return mask;
}

} // namespace

EventLoop::NotifyEntry::NotifyEntry() : node(this) {}

EventLoop::EventLoop(EventLoopGroup *group, std::size_t group_index) : group_(group), group_index_(group_index) {
    detail::queue_init(&local_queue_);
    wakeup_entry_.loop = this;
    wakeup_entry_.callback = &EventLoop::on_wakeup;
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
        return;
    }
    if (!poller_.valid()) {
        ::close(event_fd_);
        event_fd_ = -1;
        return;
    }
    if (poller_.add(event_fd_, IoEvent::Read, &wakeup_entry_) != fiber::common::IoErr::None) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

EventLoop::~EventLoop() {
    if (event_fd_ >= 0) {
        ::close(event_fd_);
    }
}

void EventLoop::notify_wakeup() {
    if (event_fd_ < 0) {
        return;
    }
    if (!wakeup_pending_.exchange(true, std::memory_order_acq_rel)) {
        std::uint64_t one = 1;
        ssize_t written = ::write(event_fd_, &one, sizeof(one));
        (void) written;
    }
}

void EventLoop::enqueue_notify(NotifyNode *node) {
    notify_queue_.push(node);
    notify_wakeup();
}

void EventLoop::on_wakeup(Poller::Item *item, int fd, IoEvent events) {
    (void) fd;
    (void) events;
    auto *entry = static_cast<WakeupEntry *>(item);
    if (!entry || !entry->loop) {
        return;
    }
    entry->loop->drain_wakeup();
}

void EventLoop::drain_wakeup() {
    if (event_fd_ < 0) {
        return;
    }
    std::uint64_t value = 0;
    for (;;) {
        ssize_t rc = ::read(event_fd_, &value, sizeof(value));
        if (rc == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    wakeup_pending_.store(false, std::memory_order_release);
}

void EventLoop::run_due_timers(std::chrono::steady_clock::time_point now) {
    for (;;) {
        TimerEntry *entry = timers_.min();
        if (!entry || entry->deadline > now) {
            break;
        }
        timers_.remove(*entry);
        entry->in_heap_ = false;
        if (entry->callback) {
            entry->callback(entry);
        }
    }
}

std::chrono::steady_clock::time_point EventLoop::next_deadline() const {
    const TimerEntry *entry = timers_.min();
    if (!entry) {
        return std::chrono::steady_clock::time_point::max();
    }
    return entry->deadline;
}

void EventLoop::prepare_run() noexcept { stop_requested_.store(false, std::memory_order_release); }

void EventLoop::run() {
    prepare_run();
    run_prepared();
}

void EventLoop::run_prepared() {
    if (event_fd_ < 0 || !poller_.valid()) {
        return;
    }
    running_.store(true, std::memory_order_release);
    EventLoop *prev = current_;
    current_ = this;
    now_ = std::chrono::steady_clock::now();
    drain_notify<false>();
    do {
        run_once();
    } while (!stop_requested_.load(std::memory_order_acquire));
    current_ = prev;
    running_.store(false, std::memory_order_release);
}

void EventLoop::run_once() {
    if (event_fd_ < 0 || !poller_.valid()) {
        return;
    }
    now_ = std::chrono::steady_clock::now();
    run_due_timers(now_);

    drain_notify<true>();
    drain_defer<true>();
    const std::chrono::steady_clock::time_point deadline = next_deadline();
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    int count = poller_.wait(events, kMaxEvents, deadline);
    now_ = std::chrono::steady_clock::now();
    if (count < 0) {
        if (errno == EINTR) {
            return;
        }
        return;
    }

    for (int i = 0; i < count; ++i) {
        auto *item = static_cast<Poller::Item *>(events[i].data.ptr);
        IoEvent io = to_io_event(events[i].events, item ? item->interested_ : IoEvent::None);
        if (to_mask(io) == 0) {
            continue;
        }
        item->callback(item, item->fd(), io);
    }
    drain_defer<false>();
}

void EventLoop::stop() {
    stop_requested_.store(true, std::memory_order_release);
    notify_wakeup();
}

void EventLoop::post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry) {
    FIBER_ASSERT(in_loop());
    FIBER_ASSERT(!entry.in_heap_);
    FIBER_ASSERT(entry.callback != nullptr);

    entry.deadline = when;
    timers_.insert(entry);
    entry.in_heap_ = true;
}

void EventLoop::cancel(TimerEntry &entry) {
    FIBER_ASSERT(in_loop());
    if (!entry.in_heap_) {
        return;
    }
    timers_.remove(entry);
    entry.in_heap_ = false;
}

void EventLoop::cancel_quiesced(TimerEntry &entry) {
    FIBER_ASSERT(!running());
    if (!entry.in_heap_) {
        return;
    }
    timers_.remove(entry);
    entry.in_heap_ = false;
}

void EventLoop::cancel(DeferEntry &entry) {
    FIBER_ASSERT(in_loop());
    if (!entry.in_queue_) {
        return;
    }
    detail::queue_remove(&entry.node_);
    entry.in_queue_ = false;
}

} // namespace fiber::event
